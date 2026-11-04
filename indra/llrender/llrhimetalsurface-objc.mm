/**
 * @file llrhimetalsurface-objc.mm
 * @brief The Metal backend's surface
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

#include "llrhimetal-objc.h" // first: it settles BOOL

#if LL_RHI_METAL

#import <AppKit/NSView.h>
#import <AppKit/NSWindow.h>
#import <QuartzCore/CATransaction.h>

#include <algorithm>
#include <chrono>

// Nothing here is initialized dynamically at load time. AppKit's objects get messages only and no class of
// AppKit's is named, so llrender links no AppKit (the viewer does).

namespace LL::RHI
{

// The window's layer ----------------------------------------------------------------------------------

CAMetalLayer* metalLayerOf(void* window)
{
    id object = (__bridge id)window;
    if (!object)
    {
        return nil;
    }
    if ([object isKindOfClass:[CAMetalLayer class]])
    {
        return object; // a layer without a view (the tests)
    }
    if (![object respondsToSelector:@selector(layer)] || ![object respondsToSelector:@selector(window)])
    {
        return nil;
    }
    NSView* view = object;
    if (!view.layer)
    {
        // The viewer's LLOpenGLView: AppKit makes a view's backing layer in the window's next display
        // cycle after the view was made, so a device made right after the window finds none yet. The window displays
        // now, which makes it at once, and the view's setLayer: sets its contentsScale and drawableSize.
        [view.window displayIfNeeded];
    }
    CALayer* layer = view.layer;
    return [layer isKindOfClass:[CAMetalLayer class]] ? (CAMetalLayer*)layer : nil;
}

// The surface -----------------------------------------------------------------------------------------

MetalSurface::MetalSurface(MetalDevice& device, CAMetalLayer* layer, U32 width, U32 height)
:   mDevice(device), mLayer(layer), mWidth(width), mHeight(height)
{
    // The layer's configuration, without implicit animations, as the view sets its layer.
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    // On macOS the layer has no device until it is set, and needs one before its first drawable
    // (CAMetalLayer.h:60-66); its residency set is the device's (:160-171), so the device comes first.
    layer.device = device.mtlDevice();
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm; // one of the two drawable formats (:73-77)
    layer.framebufferOnly = YES;                  // the flip pass only renders into it (:79-87)
    layer.opaque = YES;                           // the default (:46-47), set explicitly
    layer.colorspace = nullptr;                   // no colour matching (:118-122)
    layer.maximumDrawableCount = 3;               // (:104-110)
    layer.allowsNextDrawableTimeout = YES;        // nil after 1 s instead of blocking forever (:147-152)
    if (width > 0 && height > 0)
    {
        // Set once; the view keeps it after. The layer ignores a 0 dimension.
        layer.drawableSize = CGSizeMake(width, height);
    }
    [CATransaction commit];
    makeBuffers(width, height);
}

MetalSurface::~MetalSurface()
{
    // The pacing in one line, which a timing run's log shows, and the drawables whose presented handler had not run
    // at close (0-3 when none leaked).
    LL_INFOS("RHI") << "Metal surface: " << mStats.presents << " presents, " << mStats.drawablesPresented
                    << " drawables presented, " << mStats.presentsPaced << " left in the back buffer for the window server, "
                    << mStats.drawableTimeouts << " drawable timeouts, " << mWindowServer->drawables.load()
                    << " drawables with the window server at close" << LL_ENDL;
    // The back buffer and its depth go with their Refs, through FrameLifecycle.
    dropCopy();
}

bool MetalSurface::makeBuffers(U32 width, U32 height)
{
    // An ordinary RGBA8Unorm texture in GL's row order and its depth, which readbacks and copies read as
    // any target's; resident from creation (createMetalTexture).
    TextureDesc color;
    color.format = Format::RGBA8Unorm;
    color.width = std::max(width, 1u);
    color.height = std::max(height, 1u);
    color.usage = TEXTURE_SAMPLED | TEXTURE_RENDER_TARGET | TEXTURE_READBACK;
    TextureDesc depth = color;
    depth.format = Format::Depth32Float;
    depth.usage = TEXTURE_RENDER_TARGET | TEXTURE_READBACK; // rawSnapshot reads the window's depth, as on GL
    Ref<Texture> newColor = mDevice.createTexture(color, "back buffer");
    Ref<Texture> newDepth = mDevice.createTexture(depth, "back buffer depth");
    if (newColor.isNull() || newDepth.isNull())
    {
        return false; // createTexture wrote the WARN line
    }
    mColor = newColor;
    mDepth = newDepth;
    return true;
}

void MetalSurface::resize(U32 width, U32 height)
{
    // The frame being recorded keeps its buffers (GL's resize frame draws into the old one until its swap);
    // present() applies the size after its flip pass.
    mResizePending = true;
    mPendingWidth = width;
    mPendingHeight = height;
}

void MetalSurface::setVSync(bool enabled)
{
    mLayer.displaySyncEnabled = enabled ? YES : NO; // CAMetalLayer.h:140-145
}

namespace
{
    U64 steadyNowNs()
    {
        return U64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                       .count());
    }
}

bool metalPresentPaced(bool displaySync, bool drawableReadback, U32 withWindowServer, U32 drawableCount,
                       U64 sinceProgressNs, U64 staleNs)
{
    // With displaySync on the layer paces at the display's refresh as GL's swap interval 1 does, and nextDrawable's
    // wait is that pacing (CAMetalLayer.h:95-102, 140-145). A drawable readback needs the drawable of this frame.
    if (displaySync || drawableReadback || drawableCount == 0)
    {
        return false;
    }
    // The drawable on screen stays the layer's until the next one replaces it (CAMetalLayer.h:28-31), so with the
    // window server holding the others, nextDrawable has none to give until its next composite.
    return withWindowServer + 1 >= drawableCount && sinceProgressNs < staleNs;
}

void MetalSurface::holdDrawablesForTest(U32 count)
{
    // Added to the real ones, which only their own handlers take back, so the count never goes below 0.
    mWindowServer->drawables += count;
    mWindowServer->progressNs.store(steadyNowNs());
}

void MetalSurface::setStaleLimitForTest(U64 staleNs)
{
    mStaleNs = staleNs;
}

void MetalSurface::present()
{
    ++mStats.presents;
    // The surface's own size is the one 0x0 guard: the layer ignores a 0 dimension and would give a drawable of
    // its old size. The size checked is the one the frame is to have.
    const U32 width = mResizePending ? mPendingWidth : mWidth;
    const U32 height = mResizePending ? mPendingHeight : mHeight;
    const bool displaySync = mLayer.displaySyncEnabled;
    const U64 now = steadyNowNs();
    const U64 progress = mWindowServer->progressNs.load();
    if (width == 0 || height == 0)
    {
        ++mStats.presentsSkippedZero;
    }
    else if (metalPresentPaced(displaySync, mCopyNext, mWindowServer->drawables.load(), U32(mLayer.maximumDrawableCount),
                               now > progress ? now - progress : 0, mStaleNs))
    {
        // [Viewer Lab] The frame stays in the back buffer, and endFrame's submit sends its work as for any frame; the
        // next present gives the window server the newest frame.
        ++mStats.presentsPaced;
    }
    else
    {
        // [Viewer Lab] The startup states swap many times inside one main-loop frame (display_startup;
        // STATE_WORLD_INIT 19 times), and a drawable held until endFrame's submit ran the layer's three out after
        // three swaps, then waited 1 s for each (about 16 s). So present() sends the flip pass at once, and the
        // drawable lives in its own pool, not the main loop's, so the layer has it back as soon as it is shown.
        @autoreleasepool
        {
            IdCAMetalDrawable drawable = [mLayer nextDrawable]; // blocks; nil after the timeout (CAMetalLayer.h:95-102)
            if (drawable)
            {
                if (mDevice.metalEncoder().presentDraw(drawable.texture, mColor.get()))
                {
                    if (mCopyNext)
                    {
                        copyDrawable(drawable.texture);
                        mCopyNext = false;
                    }
                    if (!displaySync)
                    {
                        // The window server has it until its handler runs. MTLDrawable.h promises the call when it is
                        // presented on screen; it also comes for dropped ones, with presentedTime 0, and 0-3 stay
                        // pending over long runs. Added before its present. The time is taken now, after
                        // nextDrawable's wait, so a handler that ran during the wait is not set back.
                        std::shared_ptr<WindowServerState> state = mWindowServer;
                        ++state->drawables;
                        state->progressNs.store(steadyNowNs());
                        [drawable addPresentedHandler:^(IdMTLDrawable) {
                            --state->drawables;
                            state->progressNs.store(steadyNowNs());
                        }];
                    }
                    mDevice.setPendingDrawable(drawable);
                    mDevice.submitPending(); // now, not at endFrame (render-1)
                }
                // Otherwise presentDraw wrote the WARN line, and the drawable goes back to the layer unpresented.
            }
            else
            {
                // Limited as the failure lines (MetalFailureLog), written at the frame's end
                mDevice.failureLog().add(MetalFailureLog::Kind::Drawable,
                                         "Frame " + std::to_string(mDevice.frameNumber()) +
                                             ": the layer gave no drawable within its 1 s timeout; nothing is presented");
                ++mStats.drawableTimeouts;
            }
        }
    }
    if (mResizePending)
    {
        // After the flip pass, at the frame boundary; a size the surface has already makes nothing.
        mResizePending = false;
        if ((mPendingWidth != mWidth || mPendingHeight != mHeight) && makeBuffers(mPendingWidth, mPendingHeight))
        {
            mWidth = mPendingWidth;
            mHeight = mPendingHeight;
        }
    }
}

MetalSurfaceStats MetalSurface::stats() const
{
    MetalSurfaceStats stats = mStats;
    stats.drawablesWithWindowServer = mWindowServer->drawables.load();
    stats.displaySync = mLayer.displaySyncEnabled;
    const CGSize drawable = mLayer.drawableSize;
    stats.drawableWidth = U32(drawable.width);
    stats.drawableHeight = U32(drawable.height);
    stats.contentsScale = F32(mLayer.contentsScale);
    // The layer's configuration as the layer has it now, after AppKit had it (the viewer's view).
    stats.opaque = mLayer.opaque;                    // CAMetalLayer.h:46-47
    stats.colorspace = mLayer.colorspace != nullptr; // nil: no colour matching (:118-122)
    stats.pixelFormat = U32(mLayer.pixelFormat);     // :77; MTLPixelFormatBGRA8Unorm is 80 (MTLPixelFormat.h:103)
    return stats;
}

void MetalSurface::setDrawableReadback(bool on)
{
    // CAMetalLayer.h:79-87: NO lets the drawable's texture be read; it changes no pixel.
    mLayer.framebufferOnly = on ? NO : YES;
    mCopyNext = on;
    if (!on)
    {
        dropCopy();
    }
}

void MetalSurface::copyDrawable(IdMTLTexture drawable)
{
    dropCopy();
    const U32 width = U32(drawable.width);
    const U32 height = U32(drawable.height);
    IdMTLBuffer buffer = mDevice.newReadbackBuffer(U64(width) * height * 4);
    if (!buffer)
    {
        return; // newReadbackBuffer wrote the WARN line
    }
    // Tight rows of the drawable's BGRA8Unorm texels, never flipped, on the compute encoder of copies after the flip
    // pass's encoder, whose queue barrier orders it after the flip; stamped by the submit that sends it.
    const MetalCopy copy{ drawable, 0, 0, { 0, 0, width, height }, buffer, width * 4, MTLBlitOptionNone,
                          std::make_shared<MetalReadbackStamp>() };
    mDevice.encodeCopy(copy);
    mCopyBuffer = buffer;
    mCopyStamp = copy.stamp;
    mCopyWidth = width;
    mCopyHeight = height;
}

void MetalSurface::dropCopy()
{
    if (mCopyBuffer)
    {
        // The copy can still be on its way, so the buffer leaves the residency set once the GPU has finished the frame.
        MetalDevice* device = &mDevice;
        IdMTLBuffer buffer = mCopyBuffer;
        mDevice.lifecycle().release([device, buffer]() { device->residency().retire(buffer); });
    }
    mCopyBuffer = nil;
    mCopyStamp.reset();
    mCopyWidth = 0;
    mCopyHeight = 0;
}

bool MetalSurface::readDrawable(std::vector<U8>& pixels, U32& width, U32& height)
{
    pixels.clear();
    width = 0;
    height = 0;
    if (!mCopyBuffer)
    {
        return false;
    }
    // As a texture readback's wait(): a copy still being recorded is sent first.
    if (!mCopyStamp->serial)
    {
        mDevice.submitPending();
    }
    const char* why = nullptr;
    if (!mCopyStamp->serial)
    {
        why = "its copy was not sent";
    }
    else if (![mCopyStamp->event waitUntilSignaledValue:mCopyStamp->serial timeoutMS:METAL_WAIT_MS])
    {
        why = "the GPU did not finish its copy within 5000 ms";
    }
    else if (!mDevice.waitForFeedback(mCopyStamp->submission))
    {
        why = "its command buffer failed or was not reported within 5000 ms";
    }
    if (why)
    {
        LL_WARNS("RHI") << "Drawable readback: " << why << "; it has no data" << LL_ENDL;
        return false;
    }
    // The layer's BGRA8Unorm as RGBA8, the rows in the drawable's memory order: its top row first.
    const U8* bytes = static_cast<const U8*>(mCopyBuffer.contents);
    pixels.resize(size_t(mCopyWidth) * mCopyHeight * 4);
    for (size_t at = 0; at < pixels.size(); at += 4)
    {
        pixels[at] = bytes[at + 2];
        pixels[at + 1] = bytes[at + 1];
        pixels[at + 2] = bytes[at];
        pixels[at + 3] = bytes[at + 3];
    }
    width = mCopyWidth;
    height = mCopyHeight;
    return true;
}

// The interface for the tests and the golden hook (llrhimetal.h) -------------------------------------

namespace
{
    API_AVAILABLE(macos(26.0)) MetalSurface* surfaceOf(Device& device)
    {
        return device.backend() == BackendType::Metal ? static_cast<MetalDevice&>(device).metalSurface() : nullptr;
    }
}

// A device of the Metal backend exists only after metalBackend's availability check, so these take its availability.
API_AVAILABLE(macos(26.0)) MetalSurfaceStats metalSurfaceStats(Device& device)
{
    MetalSurface* surface = surfaceOf(device);
    return surface ? surface->stats() : MetalSurfaceStats();
}

API_AVAILABLE(macos(26.0)) MetalEncoderStats metalEncoderStats(Device& device)
{
    // The main thread's encoder, with or without a surface.
    return device.backend() == BackendType::Metal ? static_cast<MetalDevice&>(device).metalEncoder().stats()
                                                  : MetalEncoderStats();
}

API_AVAILABLE(macos(26.0)) MetalCompileStats metalCompileStats(Device& device)
{
    // The pipeline states' counts, PipelineCache's size and the calling threads' times.
    if (device.backend() != BackendType::Metal)
    {
        return MetalCompileStats();
    }
    MetalDevice& metal = static_cast<MetalDevice&>(device);
    MetalCompileStats stats = metal.pipelineStates().stats();
    stats.cached = metal.pipelines().size();
    stats.callerTranslations = metal.translatedOnCaller();
    const std::shared_ptr<MetalCompileState> state = metal.compileState();
    stats.callerTranslationUs = state->callerTranslationNs.load(std::memory_order_relaxed) / 1000;
    stats.programWaitUs = state->programWaitNs.load(std::memory_order_relaxed) / 1000;
    stats.pipelineWaitUs = state->pipelineWaitNs.load(std::memory_order_relaxed) / 1000;
    stats.pointWaitUs = state->pointWaitNs.load(std::memory_order_relaxed) / 1000;
    stats.failedPrograms = state->failedPrograms.load(std::memory_order_relaxed);
    stats.faultedPrograms = state->faultedPrograms.load(std::memory_order_relaxed);
    stats.faultPending = state->faultPendingAnswers.load(std::memory_order_relaxed);
    // The waits by kind, the groups and the knob
    for (size_t kind = 0; kind < METAL_WAIT_KINDS; ++kind)
    {
        stats.waits[kind].count = state->waitCount[kind].load(std::memory_order_relaxed);
        stats.waits[kind].totalUs = state->waitKindNs[kind].load(std::memory_order_relaxed) / 1000;
        stats.waits[kind].longestUs = state->longestWaitNs[kind].load(std::memory_order_relaxed) / 1000;
    }
    stats.groupsSent = state->groupsSent.load(std::memory_order_relaxed);
    stats.groupsFromMain = state->groupsFromMain.load(std::memory_order_relaxed);
    stats.groupsDone = state->groupsDone.load(std::memory_order_relaxed);
    stats.groupLatencyUs = state->groupLatencyNs.load(std::memory_order_relaxed) / 1000;
    stats.groupLatencyMaxUs = state->groupLatencyMaxNs.load(std::memory_order_relaxed) / 1000;
    stats.groupHandlerQos = state->groupHandlerQos.load(std::memory_order_relaxed);
    stats.groupHash = metal.programGroups().digestSum(); // in any order: the same groups give the same sum
    stats.poolQos = metal.programPool().qos();
    stats.compileSlow = metal.compileSlow();
    return stats;
}

bool metalCompileStalled(const MetalCompileStats& stats, U64 thresholdUs)
{
    // One wait, not their sum: a frame's many short waits are hitches, not stalls
    return std::any_of(stats.waits.begin(), stats.waits.end(),
                       [thresholdUs](const MetalWaits& waits) { return waits.longestUs >= thresholdUs; });
}

U32 metalQosBit(U32 qosClass)
{
    switch (qosClass)
    {
    case QOS_CLASS_USER_INTERACTIVE: return 1u << 0;
    case QOS_CLASS_USER_INITIATED: return 1u << 1;
    case QOS_CLASS_DEFAULT: return 1u << 2;
    case QOS_CLASS_UTILITY: return 1u << 3;
    case QOS_CLASS_BACKGROUND: return 1u << 4;
    default: return 1u << 5; // unspecified
    }
}

std::string metalQosText(U32 qosClass)
{
    switch (qosClass)
    {
    case QOS_CLASS_USER_INTERACTIVE: return "user-interactive";
    case QOS_CLASS_USER_INITIATED: return "user-initiated";
    case QOS_CLASS_DEFAULT: return "default";
    case QOS_CLASS_UTILITY: return "utility";
    case QOS_CLASS_BACKGROUND: return "background";
    default: return "unspecified";
    }
}

std::string metalQosMaskText(U32 mask)
{
    std::string text;
    for (const U32 qos : { U32(QOS_CLASS_USER_INTERACTIVE), U32(QOS_CLASS_USER_INITIATED), U32(QOS_CLASS_DEFAULT),
                           U32(QOS_CLASS_UTILITY), U32(QOS_CLASS_BACKGROUND), U32(QOS_CLASS_UNSPECIFIED) })
    {
        if (mask & metalQosBit(qos))
        {
            text += (text.empty() ? "" : "+") + metalQosText(qos);
        }
    }
    return text.empty() ? "none" : text;
}

std::string metalCompileStallText(const MetalCompileStats& stats)
{
    // One line, in ms with one decimal; the kinds in MetalWaitKind's order
    constexpr const char* KINDS[METAL_WAIT_KINDS] = { "program", "point", "specialized", "full", "variant" };
    const auto ms = [](U64 us)
    {
        char text[32];
        std::snprintf(text, sizeof(text), "%.1f", us / 1000.0);
        return std::string(text);
    };
    std::string text = "waits (count/total ms/longest ms):";
    for (size_t kind = 0; kind < METAL_WAIT_KINDS; ++kind)
    {
        const MetalWaits& waits = stats.waits[kind];
        text += std::string(kind ? ", " : " ") + KINDS[kind] + " " + std::to_string(waits.count) + "/" +
                ms(waits.totalUs) + "/" + ms(waits.longestUs);
    }
    text += "; translations on the caller " + std::to_string(stats.callerTranslations) + " (" +
            ms(stats.callerTranslationUs) + " ms)";
    text += "; groups " + std::to_string(stats.groupsSent) + " sent (" + std::to_string(stats.groupsFromMain) +
            " from the main thread), " + std::to_string(stats.groupsDone) + " done, latency " +
            ms(stats.groupLatencyUs) + " ms in all, longest " + ms(stats.groupLatencyMaxUs) + " ms, handlers at " +
            metalQosMaskText(stats.groupHandlerQos) + ", hash ";
    char hash[24];
    std::snprintf(hash, sizeof(hash), "%016llx", (unsigned long long)stats.groupHash);
    text += hash;
    text += "; translation threads at " + metalQosText(stats.poolQos) + ", LL_METAL_COMPILE_SLOW " +
            (stats.compileSlow ? "on" : "off");
    return text;
}

API_AVAILABLE(macos(26.0)) size_t metalSamplerCount(Device& device)
{
    return device.backend() == BackendType::Metal ? static_cast<MetalDevice&>(device).samplerCount() : 0;
}

API_AVAILABLE(macos(26.0)) void setMetalDrawableReadback(Device& device, bool on)
{
    if (MetalSurface* surface = surfaceOf(device))
    {
        surface->setDrawableReadback(on);
    }
}

API_AVAILABLE(macos(26.0)) bool readMetalDrawable(Device& device, std::vector<U8>& pixels, U32& width, U32& height)
{
    MetalSurface* surface = surfaceOf(device);
    if (!surface)
    {
        pixels.clear();
        width = 0;
        height = 0;
        return false;
    }
    return surface->readDrawable(pixels, width, height);
}

} // namespace LL::RHI

#endif // LL_RHI_METAL
