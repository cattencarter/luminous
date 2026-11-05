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
#import <AppKit/NSScreen.h>
#import <QuartzCore/CABase.h>
#import <QuartzCore/CATransaction.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>

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
    layer.displaySyncEnabled = YES;               // whole frames at the refresh, VSync on or off (:140-145)
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
                    << " drawables presented, " << mStats.presentsPaced << " left in the back buffer for the window server ("
                    << mStats.presentsWindowHidden << " while the window was hidden, " << mStats.presentsAfterTimeout
                    << " after a drawable timeout), " << mStats.drawableTimeouts << " drawable timeouts, "
                    << mWindowServer->drawables.load()
                    << " drawables with the window server at close; finished frames: " << mStats.finishedFlips
                    << " flipped, " << mStats.finishedSkipped << " skipped (none free), " << mStats.finishedPresented
                    << " presented, " << mStats.finishedHeld << " held, " << mStats.finishedTimeouts
                    << " drawable timeouts; nextDrawable " << mStats.nextDrawableNs / 1000000
                    << " ms in all, " << mStats.nextDrawableOver1ms << " calls over 1 ms, the longest "
                    << mStats.nextDrawableLongNs / 1000 << " us; VSync " << (mVSync ? "on" : "off") << ", layer "
                    << (mLayer.displaySyncEnabled ? "display synced" : "unsynced") << LL_ENDL;
    // The back buffer and its depth go with their Refs, through FrameLifecycle.
    dropCopy();
    dropFinished();
    if (mPresentQueue)
    {
        mDevice.residency().detachSecond(mPresentQueue, mLayer);
    }
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
    // The layer stays display synced (CAMetalLayer.h:140-145): the window server shows whole frames at the refresh.
    // VSync off only paces present() by the drawables the window server holds (metalPresentPaced), so the main loop
    // does not wait in nextDrawable for the refresh.
    mVSync = enabled;
    if (metalPresentUnsyncedKnob())
    {
        mLayer.displaySyncEnabled = enabled ? YES : NO;
    }
    if (!finishedPath())
    {
        dropFinished(); // a no-op without finished frames
    }
    applyFramebufferOnly();
}

bool metalPresentUnsyncedKnob()
{
    // A function-local static, made at the first use (nothing at load time).
    static const bool unsynced = []()
    {
        const char* knob = getenv("LL_METAL_PRESENT_UNSYNCED");
        return knob && std::strcmp(knob, "1") == 0;
    }();
    return unsynced;
}

bool metalPresentTraceKnob()
{
    static const bool trace = []()
    {
        const char* knob = getenv("LL_METAL_PRESENT_TRACE");
        return knob && std::strcmp(knob, "1") == 0;
    }();
    return trace;
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

bool metalPresentPaced(bool vsync, bool drawableReadback, U32 withWindowServer, U32 drawableCount,
                       U64 sinceProgressNs, U64 staleNs)
{
    // With VSync on the display synced layer paces at the display's refresh as GL's swap interval 1 does, and
    // nextDrawable's wait is that pacing (CAMetalLayer.h:95-102, 140-145). A drawable readback needs the drawable of
    // this frame.
    if (vsync || drawableReadback || drawableCount == 0)
    {
        return false;
    }
    // The drawable on screen stays the layer's until the next one replaces it (CAMetalLayer.h:28-31), so with the
    // window server holding the others, nextDrawable has none to give until its next composite. (Holding at one
    // queued drawable instead halved the frames that reached the screen: a drawable is shown only once
    // the GPU has finished its frame, two or three frames after its present, so the presented handlers come late.)
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

void MetalSurface::refuseDrawableForTest(U64 agoNs)
{
    // As the nil branch in present() records it: the ask began and gave up agoNs ago.
    const U64 now = steadyNowNs();
    mRefusedNs = now > agoNs ? now - agoNs : 1;
    mRefusedAskNs = mRefusedNs;
}

void MetalSurface::setWindowHiddenForTest(bool hidden)
{
    mHiddenForTest = hidden;
}

bool MetalSurface::windowVisible() const
{
    if (mHiddenForTest)
    {
        return false;
    }
    // AppKit makes a layer-backed view its layer's delegate; its window's occlusion state says whether any of the
    // window shows (NSWindow.h, NSWindowOcclusionStateVisible). A layer without a view (the tests), or a delegate
    // that is no view, counts as visible.
    ::id delegate = mLayer.delegate; // the global type: MetalSurface has an id() of its own
    if (!delegate || ![delegate respondsToSelector:@selector(window)])
    {
        return true;
    }
    NSView* view = delegate;
    NSWindow* window = view.window;
    if (!window || ![window respondsToSelector:@selector(occlusionState)])
    {
        return true;
    }
    return (window.occlusionState & NSWindowOcclusionStateVisible) != 0;
}

bool MetalSurface::presentWithheld(U64 now, U64 progress, bool stalled)
{
    // A window none of which shows composites nothing, so the window server may keep the drawables and the stale ask
    // below would wait in nextDrawable. Read only once it has stalled (stalled: drawables queued and no presented
    // handler within the stale limit): while the handlers run the ask does not block, so a wrong occlusion state
    // costs nothing. While hidden, one ask goes
    // every METAL_HIDDEN_RETRY_NS, the first that long after the stall was seen, so a state that stays wrong cannot
    // hold the picture for good.
    const bool hidden = stalled && !windowVisible();
    if (!hidden)
    {
        mHiddenAskNs = 0;
    }
    else
    {
        if (mHiddenAskNs == 0)
        {
            mHiddenAskNs = now;
        }
        if ((now > mHiddenAskNs ? now - mHiddenAskNs : 0) < METAL_HIDDEN_RETRY_NS)
        {
            ++mStats.presentsWindowHidden;
            return true;
        }
        mHiddenAskNs = now;
    }
    if (mRefusedNs == 0)
    {
        return false;
    }
    // After nextDrawable gave nil, no drawable is asked for until a presented handler has run since that ask
    // (the window server takes drawables again) or METAL_DRAWABLE_RETRY_NS has passed since it gave up.
    if (progress > mRefusedAskNs)
    {
        mRefusedNs = 0;
        mRefusedAskNs = 0;
        return false;
    }
    if ((now > mRefusedNs ? now - mRefusedNs : 0) < METAL_DRAWABLE_RETRY_NS)
    {
        ++mStats.presentsAfterTimeout;
        return true;
    }
    return false;
}

std::shared_ptr<MetalSurface::PresentTraceRecord> MetalSurface::watchDrawable(IdCAMetalDrawable drawable, bool vsync)
{
    if (!vsync)
    {
        // The window server has it until its handler runs. MTLDrawable.h promises the call when it is presented on
        // screen; it also comes for dropped ones, with presentedTime 0, and 0-3 stay pending over long runs. Added
        // before its present. The time is taken now, after nextDrawable's wait, so a handler that ran during the wait
        // is not set back.
        std::shared_ptr<WindowServerState> state = mWindowServer;
        ++state->drawables;
        state->progressNs.store(steadyNowNs());
        [drawable addPresentedHandler:^(IdMTLDrawable) {
            --state->drawables;
            state->progressNs.store(steadyNowNs());
        }];
    }
    if (!metalPresentTraceKnob() || !mTrace)
    {
        return nullptr;
    }
    // The trace's record: its present, and the submission whose feedback gives the GPU's end of its frame (known only
    // after the submit; the handler comes after the present).
    ++mTraceWindow.drawables;
    std::shared_ptr<PresentTraceRecord> record = std::make_shared<PresentTraceRecord>();
    record->presentT = CACurrentMediaTime();
    std::shared_ptr<PresentTraceState> traceState = mTrace;
    [drawable addPresentedHandler:^(IdMTLDrawable presented) {
        const double handlerT = CACurrentMediaTime();
        const double glass = presented.presentedTime;
        double gpuEnd = 0.0;
        double presentT = 0.0;
        {
            std::lock_guard<std::mutex> lock(record->mutex);
            gpuEnd = record->submission ? record->submission->gpuEnd.load() : 0.0;
            presentT = record->presentT;
        }
        std::lock_guard<std::mutex> lock(traceState->mutex);
        PresentTraceCounts& t = traceState->counts;
        const auto add = [](double& sum, double& max, double ms)
        {
            sum += ms;
            max = std::max(max, ms);
        };
        ++t.handlers;
        add(t.toHandlerSum, t.toHandlerMax, (handlerT - presentT) * 1000.0);
        if (glass > 0.0)
        {
            ++t.shown;
            add(t.toGlassSum, t.toGlassMax, (glass - presentT) * 1000.0);
            if (gpuEnd > 0.0)
            {
                ++t.gpuKnown;
                add(t.toGpuSum, t.toGpuMax, (gpuEnd - presentT) * 1000.0);
                add(t.gpuToGlassSum, t.gpuToGlassMax, (glass - gpuEnd) * 1000.0);
            }
            if (traceState->lastGlass > 0.0 && traceState->refresh > 0.0)
            {
                const double refreshes = (glass - traceState->lastGlass) / traceState->refresh;
                t.gaps[refreshes < 1.5 ? 0 : refreshes < 2.5 ? 1 : refreshes < 3.5 ? 2 : 3] += 1;
            }
            traceState->lastGlass = glass;
        }
    }];
    return record;
}

void MetalSurface::setPresentDirect(bool direct)
{
    mPresentDirect = direct;
    if (!finishedPath())
    {
        dropFinished();
    }
    applyFramebufferOnly();
}

void MetalSurface::applyFramebufferOnly()
{
    // CAMetalLayer.h:79-87: NO lets a drawable's texture be read (a drawable readback) or be a copy's destination (the
    // finished path's copy, MTL4ComputeCommandEncoder.h:186-189); it changes no pixel.
    const BOOL only = (mCopyNext || finishedPath()) ? NO : YES;
    if (mLayer.framebufferOnly != only)
    {
        mLayer.framebufferOnly = only;
    }
}

bool MetalSurface::makePresentQueue()
{
    if (mPresentQueue)
    {
        return true;
    }
    if (mPresentQueueFailed)
    {
        return false;
    }
    mPresentQueueFailed = true; // until it is made
    IdMTLDevice device = mDevice.mtlDevice();
    MTL4CommandQueueDescriptor* descriptor = [MTL4CommandQueueDescriptor new];
    descriptor.label = @"Viewer Lab present";
    NSError* error = nil;
    IdMTL4CommandQueue queue = [device newMTL4CommandQueueWithDescriptor:descriptor error:&error];
    IdMTLSharedEvent event = [device newSharedEvent];
    if (!queue || !event)
    {
        LL_WARNS("RHI") << "VSync off: Metal gave no present queue ("
                        << (error ? error.description.UTF8String : "no queue or event")
                        << "); each frame presents its own drawable" << LL_ENDL;
        return false;
    }
    for (size_t i = 0; i < mPresentAllocators.size(); ++i)
    {
        mPresentAllocators[i] = [device newCommandAllocator];
        if (!mPresentAllocators[i])
        {
            LL_WARNS("RHI") << "VSync off: Metal gave no command allocator for the present queue; each frame presents "
                               "its own drawable"
                            << LL_ENDL;
            return false;
        }
    }
    if (!mDevice.residency().attachSecond(queue, mLayer))
    {
        return false; // attachSecond wrote the WARN line
    }
    mPresentQueue = queue;
    mPresentEvent = event;
    mPresentQueueFailed = false;
    return true;
}

bool MetalSurface::finishedOnGpu(const FinishedFrame& slot) const
{
    return slot.stamp && slot.stamp->serial != 0 && slot.stamp->event.signaledValue >= slot.stamp->serial;
}

void MetalSurface::pollFinished()
{
    const U64 copied = mPresentEvent ? mPresentEvent.signaledValue : 0;
    U64 newest = 0;
    for (FinishedFrame& slot : mFinished)
    {
        if (slot.copySerial != 0 && copied >= slot.copySerial)
        {
            slot.copySerial = 0;
        }
        if (slot.flip != 0 && finishedOnGpu(slot))
        {
            newest = std::max(newest, slot.flip);
        }
    }
    // A finished frame older than a newer finished one, or no newer than the one shown last, is never shown. One the
    // GPU has not finished keeps its slot: the queue runs the flips in order, so it is newer than every finished one.
    for (FinishedFrame& slot : mFinished)
    {
        if (slot.flip != 0 && finishedOnGpu(slot) && (slot.flip < newest || slot.flip <= mShownFlip))
        {
            slot.flip = 0;
            slot.stamp.reset();
        }
    }
}

bool MetalSurface::flipFinished()
{
    pollFinished();
    // The layer's drawableSize, unless a drawable has shown that its texture's size differs (presentFinished).
    const CGSize size = mLayer.drawableSize;
    if (mFinishedSize.layerWidth != U32(size.width) || mFinishedSize.layerHeight != U32(size.height))
    {
        mFinishedSize = { U32(size.width), U32(size.height), U32(size.width), U32(size.height) };
    }
    const U32 width = mFinishedSize.width;
    const U32 height = mFinishedSize.height;
    FinishedFrame* free = nullptr;
    for (FinishedFrame& slot : mFinished)
    {
        if (slot.flip == 0 && slot.copySerial == 0)
        {
            free = &slot;
            break;
        }
    }
    if (!free || width == 0 || height == 0)
    {
        return false;
    }
    if (!free->texture || free->width != width || free->height != height)
    {
        // The drawable's layout, so the present queue's copy is a plain one into it.
        retireFinished(*free);
        MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                        width:width
                                                                                       height:height
                                                                                    mipmapped:NO];
        desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        desc.storageMode = MTLStorageModePrivate;
        IdMTLTexture texture = [mDevice.mtlDevice() newTextureWithDescriptor:desc];
        if (!texture)
        {
            LL_WARNS("RHI") << "VSync off: Metal gave no " << width << "x" << height << " finished frame" << LL_ENDL;
            return false;
        }
        texture.label = @"finished frame";
        mDevice.residency().admit(texture, "finished frame"); // committed before the submit that flips into it
        free->texture = texture;
        free->width = width;
        free->height = height;
    }
    if (!mDevice.metalEncoder().presentDraw(free->texture, mColor.get()))
    {
        return false; // presentDraw wrote the WARN line
    }
    free->flip = ++mFlips;
    free->flipT = metalPresentTraceKnob() ? CACurrentMediaTime() : 0.0;
    free->stamp = std::make_shared<MetalReadbackStamp>();
    mDevice.stampNextSubmit(free->stamp);
    mDevice.submitPending(); // now, as a drawable's flip pass
    return true;
}

void MetalSurface::retireFinished(FinishedFrame& slot)
{
    if (slot.texture)
    {
        // No flip into it and no copy from it is pending (a free slot, or after dropFinished's wait); the frame
        // lifecycle still keeps it resident until the GPU has finished what was sent before.
        MetalDevice* device = &mDevice;
        IdMTLTexture texture = slot.texture;
        mDevice.lifecycle().release([device, texture]() { device->residency().retire(texture); });
    }
    slot = FinishedFrame();
}

void MetalSurface::dropFinished()
{
    mShownFlip = mFlips;
    if (mPresentEvent && mPresentSerial != 0 && mPresentEvent.signaledValue < mPresentSerial &&
        ![mPresentEvent waitUntilSignaledValue:mPresentSerial timeoutMS:METAL_WAIT_MS])
    {
        // A copy may still read them, so the finished frames stay resident (and are not reused: none is shown).
        LL_WARNS("RHI") << "VSync off: the present queue did not finish its copies within " << METAL_WAIT_MS
                        << " ms; its finished frames are kept" << LL_ENDL;
        return;
    }
    for (FinishedFrame& slot : mFinished)
    {
        retireFinished(slot);
    }
}

bool MetalSurface::presentFinished(U64 now, U64 progress, bool justFlipped)
{
    if (mDevice.deviceLost())
    {
        return false;
    }
    pollFinished();
    FinishedFrame* newest = nullptr;
    U32 pending = 0;
    for (FinishedFrame& slot : mFinished)
    {
        const bool finished = finishedOnGpu(slot);
        pending += slot.flip != 0 && !finished ? 1 : 0;
        if (slot.flip > mShownFlip && (finished || justFlipped) && (!newest || slot.flip > newest->flip))
        {
            newest = &slot;
        }
    }
    if (!newest || (justFlipped && (newest->flip != mFlips || pending > (finishedOnGpu(*newest) ? 0u : 1u))))
    {
        // justFlipped: only the frame just flipped, and only while no older flip is still on the GPU (a CPU-bound
        // frame), so its drawable waits for that frame's own work alone; else it goes once finished, as any.
        return false;
    }
    const bool trace = metalPresentTraceKnob();
    const U32 withWindowServer = mWindowServer->drawables.load();
    const U64 since = now > progress ? now - progress : 0;
    if (metalPresentPaced(false, false, withWindowServer, U32(mLayer.maximumDrawableCount), since, mStaleNs))
    {
        ++mStats.finishedHeld;
        mTraceWindow.held += trace ? 1 : 0;
        return false;
    }
    if (presentWithheld(now, progress, withWindowServer > 0 && since >= mStaleNs))
    {
        ++mStats.finishedHeld;
        mTraceWindow.withheld += trace ? 1 : 0;
        return false;
    }
    const CGSize size = mLayer.drawableSize;
    if (U32(size.width) != mFinishedSize.layerWidth || U32(size.height) != mFinishedSize.layerHeight ||
        newest->width != mFinishedSize.width || newest->height != mFinishedSize.height)
    {
        // Flipped before the layer's size changed: a drawable has the new size, so no copy fits. The next frame's does.
        newest->flip = 0;
        newest->stamp.reset();
        return false;
    }
    const size_t a = mNextAllocator;
    if (mAllocatorSerial[a] > mPresentEvent.signaledValue)
    {
        return false; // the allocator's last copy still runs, and an allocator is reset only once its work has run
    }
    @autoreleasepool
    {
        const U64 asked = steadyNowNs();
        IdCAMetalDrawable drawable = [mLayer nextDrawable]; // blocks; nil after the timeout (CAMetalLayer.h:95-102)
        const U64 waited = steadyNowNs() - asked;
        mStats.nextDrawableNs += waited;
        mStats.nextDrawableLongNs = std::max(mStats.nextDrawableLongNs, waited);
        mStats.nextDrawableOver1ms += waited > 1000000 ? 1 : 0;
        if (trace)
        {
            mTraceWindow.nextDrawableSumNs += waited;
            mTraceWindow.nextDrawableMaxNs = std::max(mTraceWindow.nextDrawableMaxNs, waited);
        }
        if (!drawable)
        {
            mDevice.failureLog().add(MetalFailureLog::Kind::Drawable,
                                     "Frame " + std::to_string(mDevice.frameNumber()) +
                                         ": the layer gave no drawable within its 1 s timeout; nothing is presented");
            ++mStats.drawableTimeouts;
            ++mStats.finishedTimeouts;
            mRefusedAskNs = std::max<U64>(asked, 1);
            mRefusedNs = std::max<U64>(steadyNowNs(), 1);
            return false;
        }
        mRefusedNs = 0;
        mRefusedAskNs = 0;
        if (drawable.texture.width != newest->width || drawable.texture.height != newest->height)
        {
            // A drawable of another size than drawableSize said (a fractional scale's rounding): no copy fits, and
            // the drawable goes back to the layer unpresented. The next frame's finished frame takes its size.
            LL_WARNS_ONCE("RHI") << "VSync off: a drawable of " << drawable.texture.width << "x"
                                 << drawable.texture.height << " for a " << newest->width << "x" << newest->height
                                 << " finished frame; not shown" << LL_ENDL;
            mFinishedSize.width = U32(drawable.texture.width); // for this drawableSize (layerWidth x layerHeight)
            mFinishedSize.height = U32(drawable.texture.height);
            newest->flip = 0;
            newest->stamp.reset();
            return false;
        }
        IdMTL4CommandAllocator allocator = mPresentAllocators[a];
        [allocator reset];
        IdMTL4CommandBuffer buffer = [mDevice.mtlDevice() newCommandBuffer];
        [buffer beginCommandBufferWithAllocator:allocator];
        IdMTL4ComputeCommandEncoder copies = [buffer computeCommandEncoder];
        if (!copies)
        {
            [buffer endCommandBuffer];
            LL_WARNS_ONCE("RHI") << "VSync off: Metal gave no compute encoder for a finished frame's copy" << LL_ENDL;
            return false; // the drawable goes back to the layer unpresented
        }
        copies.label = @"Present a finished frame";
        // The same size and pixel format (MTL4ComputeCommandEncoder.h:166-173); the layer's framebufferOnly is NO on
        // this path (applyFramebufferOnly), so the drawable can be the copy's destination.
        [copies copyFromTexture:newest->texture toTexture:drawable.texture];
        [copies endEncoding];
        [buffer endCommandBuffer];
        std::shared_ptr<PresentTraceRecord> record = watchDrawable(drawable, false);
        // The copy runs after the flip into the finished frame (signalled already, so the GPU does not wait; the event
        // orders the two queues and makes the flip's writes visible) and once the display no longer uses the drawable
        // (MTL4CommandQueue.h:296-308); it signals the drawable before its present (:277-294).
        [mPresentQueue waitForEvent:newest->stamp->event value:newest->stamp->serial];
        [mPresentQueue waitForDrawable:drawable];
        // Its errors in the failure log as the frames' own, with what it waited for
        MTL4CommitOptions* options = [MTL4CommitOptions new];
        std::shared_ptr<MetalFailureLog> failures = mDevice.failureLogShared();
        IdMTLSharedEvent flipEvent = newest->stamp->event;
        const U64 flipSerial = newest->stamp->serial;
        const U64 frame = mDevice.frameNumber();
        const double committedT = CACurrentMediaTime();
        [options addFeedbackHandler:^(IdMTL4CommitFeedback commitFeedback) {
            if (!commitFeedback.error)
            {
                return;
            }
            char line[320];
            snprintf(line, sizeof(line),
                     "GPU error in frame %llu's present of a finished frame (VSync off): %s; committed %.2f s ago, "
                     "begun on the GPU: %s; it waited for the flip's serial %llu (signalled %llu now) and its drawable",
                     (unsigned long long)frame, metalErrorName(commitFeedback.error).c_str(),
                     CACurrentMediaTime() - committedT, commitFeedback.GPUStartTime > 0.0 ? "yes" : "no",
                     (unsigned long long)flipSerial, (unsigned long long)flipEvent.signaledValue);
            failures->add(MetalFailureLog::Kind::GpuError, line);
        }];
        [mPresentQueue commit:&buffer count:1 options:options];
        [mPresentQueue signalDrawable:drawable];
        [drawable present];
        [mPresentQueue signalEvent:mPresentEvent value:++mPresentSerial];
        if (record)
        {
            // From the frame's own present() and its submission, as on the direct path: present->glass is the frame's
            // whole delay, and GPU end->glass the shown frame's age.
            std::lock_guard<std::mutex> lock(record->mutex);
            record->presentT = newest->flipT;
            record->submission = newest->stamp->submission;
        }
    }
    newest->copySerial = mPresentSerial;
    mAllocatorSerial[a] = mPresentSerial;
    mNextAllocator = (a + 1) % mPresentAllocators.size();
    mShownFlip = newest->flip;
    newest->flip = 0;
    newest->stamp.reset();
    ++mStats.drawablesPresented;
    ++mStats.drawableWaits;
    ++mStats.drawableSignals;
    ++mStats.finishedPresented;
    return true;
}

void MetalSurface::frameEnded()
{
    // After FrameLifecycle::endFrame's wait for an earlier frame, which is when a GPU-bound frame has just finished.
    if (mPresentQueue && finishedPath() && !mCopyNext)
    {
        presentFinished(steadyNowNs(), mWindowServer->progressNs.load());
    }
}

void MetalSurface::writePresentTrace(U64 now)
{
    if (!mTrace)
    {
        mTrace = std::make_shared<PresentTraceState>();
        mTraceWindow = PresentTraceWindow();
        mTraceWindow.beganNs = now;
    }
    const U64 elapsed = now - mTraceWindow.beganNs;
    if (elapsed < 1'000'000'000)
    {
        return;
    }
    // The screen's refresh, for the gaps between shown drawables (NSScreen.maximumFramesPerSecond).
    double refresh = 1.0 / 60.0;
    ::id delegate = mLayer.delegate;
    if (delegate && [delegate respondsToSelector:@selector(window)])
    {
        NSView* view = delegate;
        NSScreen* screen = view.window.screen;
        if (screen && screen.maximumFramesPerSecond > 0)
        {
            refresh = 1.0 / double(screen.maximumFramesPerSecond);
        }
    }
    PresentTraceCounts t;
    {
        std::lock_guard<std::mutex> lock(mTrace->mutex);
        t = mTrace->counts;
        mTrace->counts = PresentTraceCounts();
        mTrace->refresh = refresh;
    }
    const PresentTraceWindow& w = mTraceWindow;
    const double seconds = double(elapsed) / 1e9;
    const auto mean = [](double sum, U32 n) { return n ? sum / n : 0.0; };
    LL_INFOS("RHI") << llformat("Metal present trace %.2f s: %u presents, %u drawables (queued at present 0/1/2/3+: "
                                "%u/%u/%u/%u), %u held, %u asked after stale, %u withheld; nextDrawable %.2f ms "
                                "in all, longest %.2f; %u handlers, %u shown (%.1f/s at %.0f Hz), gaps 1/2/3/4+ "
                                "refreshes %u/%u/%u/%u; present->glass %.1f max %.1f ms, present->GPU end %.1f max "
                                "%.1f (%u known), GPU end->glass %.1f max %.1f, present->handler %.1f max %.1f; "
                                "VSync %s",
                                seconds, w.presents, w.drawables, w.queued[0], w.queued[1], w.queued[2], w.queued[3],
                                w.held, w.staleAsks, w.withheld, double(w.nextDrawableSumNs) / 1e6,
                                double(w.nextDrawableMaxNs) / 1e6, t.handlers, t.shown, t.shown / seconds,
                                1.0 / refresh, t.gaps[0], t.gaps[1], t.gaps[2], t.gaps[3],
                                mean(t.toGlassSum, t.shown), t.toGlassMax, mean(t.toGpuSum, t.gpuKnown), t.toGpuMax,
                                t.gpuKnown, mean(t.gpuToGlassSum, t.gpuKnown), t.gpuToGlassMax,
                                mean(t.toHandlerSum, t.handlers), t.toHandlerMax, mVSync ? "on" : "off")
                    << LL_ENDL;
    mTraceWindow = PresentTraceWindow();
    mTraceWindow.beganNs = now;
}

void MetalSurface::present()
{
    ++mStats.presents;
    // The surface's own size is the one 0x0 guard: the layer ignores a 0 dimension and would give a drawable of
    // its old size. The size checked is the one the frame is to have.
    const U32 width = mResizePending ? mPendingWidth : mWidth;
    const U32 height = mResizePending ? mPendingHeight : mHeight;
    const bool vsync = mVSync;
    const U64 now = steadyNowNs();
    const U64 progress = mWindowServer->progressNs.load();
    const bool trace = metalPresentTraceKnob();
    if (trace)
    {
        writePresentTrace(now);
        ++mTraceWindow.presents;
        ++mTraceWindow.queued[std::min<U32>(mWindowServer->drawables.load(), 3)];
        if (!vsync && !mCopyNext && mWindowServer->drawables.load() + 1 >= U32(mLayer.maximumDrawableCount) &&
            (now > progress ? now - progress : 0) >= mStaleNs)
        {
            ++mTraceWindow.staleAsks;
        }
    }
    if (width == 0 || height == 0)
    {
        ++mStats.presentsSkippedZero;
    }
    else if (!vsync && !mCopyNext && !mPresentDirect && makePresentQueue())
    {
        // [Viewer Lab] <Luminous> VSync off: the window server gets the newest frame the GPU has finished, through
        // the present queue, so a drawable waits for no frame in flight (a drawable flipped at the end of its frame's
        // command buffer reached the screen 3-4 frames after its present when the GPU was the bottleneck, and three
        // drawables over that delay showed about 28 frames a second in a trace of VSync off). An older
        // finished frame goes first if the window server may take one; this frame's flip goes into a finished frame,
        // sent at once.
        presentFinished(now, progress);
        if (flipFinished())
        {
            ++mStats.finishedFlips;
            // With no older flip on the GPU (a CPU-bound frame) this one goes at once, its copy waiting on the GPU
            // for its own work only; otherwise once finished (here or at frameEnded).
            presentFinished(now, progress, true);
        }
        else
        {
            ++mStats.finishedSkipped;
        }
    }
    else if (metalPresentPaced(vsync, mCopyNext, mWindowServer->drawables.load(), U32(mLayer.maximumDrawableCount),
                               now > progress ? now - progress : 0, mStaleNs))
    {
        // [Viewer Lab] The frame stays in the back buffer, and endFrame's submit sends its work as for any frame; the
        // next present gives the window server the newest frame.
        ++mStats.presentsPaced;
        mTraceWindow.held += trace ? 1 : 0;
    }
    else if (!vsync && !mCopyNext
             && presentWithheld(now, progress,
                                mWindowServer->drawables.load() > 0 && (now > progress ? now - progress : 0) >= mStaleNs))
    {
        // VSync off, a hidden window or a layer that has just refused a drawable: left in the back buffer as a
        // paced present, without the blocking ask.
        ++mStats.presentsPaced;
        mTraceWindow.withheld += trace ? 1 : 0;
    }
    else
    {
        // [Viewer Lab] The startup states swap many times inside one main-loop frame (display_startup;
        // STATE_WORLD_INIT 19 times), and a drawable held until endFrame's submit ran the layer's three out after
        // three swaps, then waited 1 s for each (about 16 s). So present() sends the flip pass at once, and the
        // drawable lives in its own pool, not the main loop's, so the layer has it back as soon as it is shown.
        @autoreleasepool
        {
            const U64 asked = steadyNowNs();
            IdCAMetalDrawable drawable = [mLayer nextDrawable]; // blocks; nil after the timeout (CAMetalLayer.h:95-102)
            const U64 waited = steadyNowNs() - asked;
            mStats.nextDrawableNs += waited;
            mStats.nextDrawableLongNs = std::max(mStats.nextDrawableLongNs, waited);
            mStats.nextDrawableOver1ms += waited > 1000000 ? 1 : 0;
            if (trace)
            {
                mTraceWindow.nextDrawableSumNs += waited;
                mTraceWindow.nextDrawableMaxNs = std::max(mTraceWindow.nextDrawableMaxNs, waited);
            }
            if (drawable)
            {
                mRefusedNs = 0;
                mRefusedAskNs = 0;
                if (mDevice.metalEncoder().presentDraw(drawable.texture, mColor.get()))
                {
                    if (mCopyNext)
                    {
                        copyDrawable(drawable.texture);
                        mCopyNext = false;
                    }
                    std::shared_ptr<PresentTraceRecord> record = watchDrawable(drawable, vsync);
                    mDevice.setPendingDrawable(drawable);
                    mDevice.submitPending(); // now, not at endFrame (render-1)
                    // No finished frame flipped before this one is shown after it.
                    mShownFlip = mFlips;
                    if (record)
                    {
                        std::lock_guard<std::mutex> lock(record->mutex);
                        record->submission = mDevice.lastSubmission();
                    }
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
                // With VSync off the presents after it are withheld (presentWithheld).
                mRefusedAskNs = std::max<U64>(asked, 1);
                mRefusedNs = std::max<U64>(steadyNowNs(), 1);
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
    stats.displaySync = mVSync;
    stats.layerDisplaySync = mLayer.displaySyncEnabled;
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
    mCopyNext = on;
    applyFramebufferOnly();
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
    constexpr const char* KINDS[METAL_WAIT_KINDS] = { "program", "point", "full", "variant" };
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

API_AVAILABLE(macos(26.0)) void setMetalPresentDirect(Device& device, bool direct)
{
    if (MetalSurface* surface = surfaceOf(device))
    {
        surface->setPresentDirect(direct);
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
