/**
 * @file llrhimetalencoder-objc.mm
 * @brief The Metal backend's encoder, its draws and readbacks
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

#include <algorithm>
#include <cstring>
#include <numeric>
#include <optional>
#include <unordered_set>

namespace LL::RHI
{

namespace
{
    MTLLoadAction loadAction(LoadAction load)
    {
        switch (load)
        {
        case LoadAction::Load:     return MTLLoadActionLoad;
        case LoadAction::Clear:    return MTLLoadActionClear;
        case LoadAction::DontCare: return MTLLoadActionDontCare;
        }
        return MTLLoadActionLoad;
    }

    MTLStoreAction storeAction(StoreAction store)
    {
        return store == StoreAction::DontCare ? MTLStoreActionDontCare : MTLStoreActionStore;
    }

    /// A memoryless attachment (TEXTURE_MEMORYLESS) can neither load nor store: Metal refuses both
    /// (MTLResource.h:75-79). It begins every segment cleared and is never stored.
    API_AVAILABLE(macos(26.0)) bool memoryless(IdMTLTexture texture)
    {
        return texture && texture.storageMode == MTLStorageModeMemoryless;
    }

    MTLCullMode cullMode(CullMode mode)
    {
        switch (mode)
        {
        case CullMode::Off:   return MTLCullModeNone;
        case CullMode::Front: return MTLCullModeFront;
        case CullMode::Back:  return MTLCullModeBack;
        }
        return MTLCullModeNone;
    }

    /// The pass's color attachments whose GL format has no alpha (hasAlphaInGLFormat), bit i for
    /// attachment i. GL keeps their alpha 1, so draws and clears there leave alpha alone (writeMask).
    U32 alphaLessAttachments(const RenderPassDesc& pass)
    {
        U32 mask = 0;
        for (U32 i = 0; i < pass.colorCount && i < PipelineDesc::MAX_COLOR_ATTACHMENTS; ++i)
        {
            if (pass.color[i].texture.notNull() && !hasAlphaInGLFormat(pass.color[i].texture->desc().glFormat))
            {
                mask |= 1u << i;
            }
        }
        return mask;
    }

    /// The pass's attachment size: the first color attachment's level, or the depth's.
    void attachmentSize(const RenderPassDesc& pass, U32& width, U32& height)
    {
        const Texture* texture = nullptr;
        U32 level = 0;
        if (pass.colorCount > 0 && pass.color[0].texture.notNull())
        {
            texture = pass.color[0].texture.get();
            level = pass.color[0].level;
        }
        else if (pass.depth.texture.notNull())
        {
            texture = pass.depth.texture.get();
            level = pass.depth.level;
        }
        width = texture ? std::max(1u, texture->desc().width >> level) : 0;
        height = texture ? std::max(1u, texture->desc().height >> level) : 0;
    }

    /// The scissor in Metal's numbers: the whole attachment when it is off, otherwise clipped to
    /// it. The encoder's numbers count rows from row 0 of the attachment, on Metal too.
    MTLScissorRect scissorRect(const EncoderTracker::DynamicState& dynamic, U32 width, U32 height)
    {
        if (!dynamic.scissorEnabled)
        {
            return { 0, 0, width, height };
        }
        const S64 x0 = std::clamp<S64>(dynamic.scissor[0], 0, width);
        const S64 y0 = std::clamp<S64>(dynamic.scissor[1], 0, height);
        const S64 x1 = std::clamp<S64>(S64(dynamic.scissor[0]) + dynamic.scissor[2], x0, width);
        const S64 y1 = std::clamp<S64>(S64(dynamic.scissor[1]) + dynamic.scissor[3], y0, height);
        return { NSUInteger(x0), NSUInteger(y0), NSUInteger(x1 - x0), NSUInteger(y1 - y0) };
    }

    /// GL's viewport in Metal's numbers: the origin row counts from row 0 of the
    /// attachment on both, the height stays positive and depth is 0..1, because the vertex stage
    /// flips y (MTL4RenderCommandEncoder.h:88).
    MTLViewport metalViewport(const std::array<S32, 4>& viewport)
    {
        return { double(viewport[0]), double(viewport[1]), double(viewport[2]), double(viewport[3]), 0.0, 1.0 };
    }

    /// A 1-pixel horizontal line exactly on the edge between two rows (the UI's lines at whole coordinates)
    /// takes the row above the edge in GL's window framebuffer and the row below in the back buffer on Metal; in a
    /// render target GL and Metal agree (level 1's primitives scene is exact), as do vertical lines everywhere. Line
    /// draws into the back buffer move the viewport up by a 64th of a pixel (rows count up from row 0 of the
    /// attachment, as GL's), so such a tie takes GL's row. A line off an edge keeps its pixels only when it
    /// lies more than a 64th of a pixel below the next row edge: the UI's coordinates at UIScaleFactor 1.0, 1.25 and 2.0
    /// are multiples of a quarter pixel, so they do; at other UI scales (a user setting), and for any world-space line
    /// drawn into the back buffer, a line within (k + 63/64, k + 1) takes the row above GL's, about 1.6 % of positions.
    /// The faithful fix, rasterising the back buffer in the drawable's row order, would also
    /// cover the triangles' top edges on row ties and retire this shift.
    constexpr double LINE_TIE_SHIFT = 1.0 / 64.0;

    bool isLine(PrimitiveType type)
    {
        return type == PrimitiveType::Lines || type == PrimitiveType::LineStrip || type == PrimitiveType::LineLoop;
    }

    /// The primitives Metal draws as they are (MTLRenderCommandEncoder.h:31-35); points
    /// on the point pipeline. Fans, strips and loops become lists.
    std::optional<MTLPrimitiveType> nativePrimitive(PrimitiveType type)
    {
        switch (type)
        {
        case PrimitiveType::Triangles: return MTLPrimitiveTypeTriangle;
        case PrimitiveType::Lines:     return MTLPrimitiveTypeLine;
        case PrimitiveType::LineStrip: return MTLPrimitiveTypeLineStrip;
        case PrimitiveType::Points:    return MTLPrimitiveTypePoint;
        default:                       return std::nullopt;
        }
    }

    /// The columns of a matrix uniform, as the setters give them one after another; 1 for the rest.
    U32 matrixColumns(UniformType type)
    {
        switch (type)
        {
        case UniformType::Mat2:   return 2;
        case UniformType::Mat3:
        case UniformType::Mat3x4: return 3;
        case UniformType::Mat4:   return 4;
        default:                  return 1;
        }
    }
}

/// A texture readback: a Shared buffer with tight rows of texels in GL's order.
class API_AVAILABLE(macos(26.0)) MetalReadback final : public Readback
{
public:
    /// alphaOne: the texture has no alpha in GL, so the copy's alpha is made 1; type is its texels'.
    /// packRGB: the buffer holds tight RGBA8 rows, which the readback gives as tight RGB rows of rowPitch bytes.
    MetalReadback(MetalDevice& device, IdMTLBuffer buffer, U32 rowPitch, std::shared_ptr<MetalReadbackStamp> stamp,
                  bool alphaOne = false, PixelLayout::Type type = PixelLayout::Type::U8, bool packRGB = false)
    :   mDevice(device), mBuffer(buffer), mRowPitch(rowPitch), mStamp(std::move(stamp)), mAlphaOne(alphaOne && !packRGB),
        mType(type), mPackRGB(packRGB)
    {
    }

    ~MetalReadback() override
    {
        // The copy of a readback that was never waited for can still be on its way, so the buffer
        // leaves the residency set once the GPU has finished the frame (the device outlives it).
        MetalDevice* device = &mDevice;
        IdMTLBuffer buffer = mBuffer;
        mDevice.lifecycle().release([device, buffer]() { device->residency().retire(buffer); });
    }

    bool ready() const override
    {
        bool failed = false;
        return reported(failed);
    }

    void wait() override
    {
        if (mWaited)
        {
            return;
        }
        mWaited = true;
        if (!mStamp->serial)
        {
            mDevice.submitPending();
        }
        const char* why = nullptr;
        if (!mStamp->serial)
        {
            why = "its copy was not sent";
        }
        else if (![mStamp->event waitUntilSignaledValue:mStamp->serial timeoutMS:METAL_WAIT_MS])
        {
            why = "the GPU did not finish its copy within 5000 ms";
        }
        else if (!mDevice.waitForFeedback(mStamp->submission))
        {
            // The event does not order the feedback handler, so the copy counts once it has run.
            why = "its command buffer failed or was not reported within 5000 ms";
        }
        if (why)
        {
            LL_WARNS("RHI") << "Readback: " << why << "; it has no data" << LL_ENDL;
            mFailed = true;
        }
    }

    std::span<const std::byte> data() const override
    {
        bool failed = false;
        if (mFailed || !reported(failed) || failed)
        {
            return {};
        }
        if (mAlphaOne)
        {
            forceAlpha();
        }
        if (mPackRGB)
        {
            packRGB();
        }
        const size_t length = mPacked ? size_t(mBuffer.length) / 4 * 3 : size_t(mBuffer.length);
        return { static_cast<const std::byte*>(mBuffer.contents), length };
    }

    U32 rowPitch() const override { return mRowPitch; }

    IdMTLBuffer buffer() const { return mBuffer; }

private:
    /// Once the copy is reported done, alpha 1 in the Shared buffer for a texture without alpha in GL, as
    /// glReadPixels gives it, also for one no pass has loaded (RGBA8 0xFF, RGBA16F 0x3C00, RGBA32F 1.0f).
    void forceAlpha() const
    {
        mAlphaOne = false;
        std::byte* bytes = static_cast<std::byte*>(mBuffer.contents);
        const size_t length = size_t(mBuffer.length);
        switch (mType)
        {
        case PixelLayout::Type::U8:
            for (size_t at = 3; at < length; at += 4)
            {
                bytes[at] = std::byte{ 0xFF };
            }
            break;
        case PixelLayout::Type::F16:
            for (size_t at = 6; at + 2 <= length; at += 8)
            {
                const U16 one = 0x3C00;
                std::memcpy(bytes + at, &one, sizeof(one));
            }
            break;
        case PixelLayout::Type::F32:
            for (size_t at = 12; at + 4 <= length; at += 16)
            {
                const F32 one = 1.f;
                std::memcpy(bytes + at, &one, sizeof(one));
            }
            break;
        default:
            break; // no format with a type of four other components lacks alpha in GL
        }
    }

    /// Once the copy is reported done, the tight RGBA8 rows packed in place to tight RGB, as
    /// glReadPixels gives GL_RGB with GL_PACK_ALIGNMENT 1. Each pixel's three bytes move to a lower or the same
    /// offset, after every pixel before it has moved, so no pixel is overwritten before it is read.
    void packRGB() const
    {
        mPackRGB = false;
        mPacked = true;
        std::byte* bytes = static_cast<std::byte*>(mBuffer.contents);
        const size_t pixels = size_t(mBuffer.length) / 4;
        for (size_t pixel = 0; pixel < pixels; ++pixel)
        {
            std::memmove(bytes + pixel * 3, bytes + pixel * 4, 3);
        }
    }

    /// Sent, signalled and reported by its submission's feedback handler; never waits.
    bool reported(bool& failed) const
    {
        if (!mStamp->serial || mStamp->event.signaledValue < mStamp->serial)
        {
            return false;
        }
        std::shared_ptr<MetalDevice::FeedbackState> feedback = mDevice.feedbackState();
        std::lock_guard<std::mutex> lock(feedback->mutex);
        failed = mStamp->submission->failed;
        return mStamp->submission->received;
    }

    MetalDevice& mDevice;
    IdMTLBuffer mBuffer;
    const U32 mRowPitch;
    std::shared_ptr<MetalReadbackStamp> mStamp;
    bool mWaited = false;
    bool mFailed = false;
    mutable bool mAlphaOne; ///< alpha is still to be made 1
    const PixelLayout::Type mType;
    mutable bool mPackRGB;      ///< the RGBA8 rows are still to be packed to RGB
    mutable bool mPacked = false; ///< the buffer holds tight RGB rows
};

IdMTLBuffer metalBuffer(Readback* readback)
{
    return readback ? static_cast<MetalReadback*>(readback)->buffer() : nil;
}

// The encoder ---------------------------------------------------------------------------------------

MetalEncoder::MetalEncoder(MetalDevice& device, const Capabilities& caps)
:   mDevice(device), mTracker(caps, true, metalDebug()), mRecordingThread(std::this_thread::get_id())
{
    mChannelVersions.fill(mNextVersion);
}

void MetalEncoder::onRecordingThread(const char* call) const
{
    if (mDevice.debugChecks() && std::this_thread::get_id() != mRecordingThread)
    {
        LL_ERRS("RHI") << call << " on another thread than the recording thread: only the main thread records "
                       << "(decision 3)" << LL_ENDL;
    }
}

bool MetalEncoder::accept(const char* call, const std::string& why)
{
    if (why.empty())
    {
        return true;
    }
    LL_WARNS("RHI") << call << ": " << why << LL_ENDL;
    return false;
}

bool MetalEncoder::beginRenderPass(const RenderPassDesc& pass)
{
    onRecordingThread("beginRenderPass");
    // A native pass suspends an open stack pass: what the stack pass recorded is stored first, as for a copy, and
    // the stack pass goes on in a new segment that loads when the native pass ends (closePass).
    if (pass.native && mTracker.inPass() && mTracker.pass().stack && !mTracker.suspendedPass())
    {
        splitPass();
    }
    if (!accept("beginRenderPass", mTracker.beginRenderPass(pass)))
    {
        return false;
    }
    if (pass.native)
    {
        mNativeSlots[size_t(NativeStage::Vertex)] = NativeSlots();
        mNativeSlots[size_t(NativeStage::Fragment)] = NativeSlots();
    }
    // A segment without a native encoder: a clear at its start becomes a load action, and the
    // encoder is made when the pass ends or a submit splits it.
    mSegment = pass;
    mSegmentWork = false;
    return true;
}

void MetalEncoder::endRenderPass()
{
    onRecordingThread("endRenderPass");
    // LLRenderTarget ends its stack's passes itself, inside OwnGL; a stack pass ended by anyone else is
    // warned about, and still ends.
    if (mTracker.inPass() && mTracker.pass().stack && !metalFacadeClear().ownGL)
    {
        LL_WARNS("RHI") << "stack pass " << mTracker.pass().name
                        << " ended with endRenderPass; LLRenderTarget::endStackPass ends it" << LL_ENDL;
    }
    // The copies read the pass as it stands, before its store actions.
    breakForCopies();
    closePass(mTracker.endRenderPass(), "endRenderPass");
}

void MetalEncoder::endFrame()
{
    breakForCopies();
    dropUnheldSlots();
    abandonQuery(); // the tracker drops an active query at the frame's end (llrhi.cpp, EncoderTracker::endFrame)
    closePass(mTracker.endFrame(), "endFrame");
    // A depth clear no later segment of the frame took gets a pass of its own.
    settleDepthClears(nullptr);
    // The tracker and dropUnheldSlots may have let go of bindings.
    syncBlocks();
    forgetResolves();
    // The targets still waiting are cleared with the frame that ends, so the list holds none past it: one let go
    // meanwhile goes with this frame, as a deleted texture does.
    zeroNewTargets();
}

void MetalEncoder::dropUnheldSlots()
{
    // A slot's texture that nobody but the slots and the tracker holds can never be told
    // again, so the slot empties at the frame's end, as the tracker drops such a binding right after (endFrame) and GL
    // unbinds a deleted texture; otherwise a deleted render target's attachments would stay alive and resident. The
    // slots' and the tracker's own references are counted first, and every slot is judged before any is emptied.
    std::unordered_map<const Texture*, S32> held;
    for (const auto& [channel, slots] : mTypeSlots)
    {
        for (const TypeSlot& slot : slots.types)
        {
            if (slot.texture.notNull())
            {
                ++held[slot.texture.get()];
            }
        }
    }
    if (held.empty())
    {
        return;
    }
    for (const auto& [channel, texture] : mTracker.textures())
    {
        const auto found = texture.notNull() ? held.find(texture.get()) : held.end();
        if (found != held.end())
        {
            ++found->second;
        }
    }
    // Judged in a pass of their own, as the tracker collects its drops first: emptying a slot drops a reference, so a
    // texture on two slots would otherwise look held to the second and stay a frame longer.
    std::unordered_set<const Texture*> unheld;
    for (const auto& [texture, refs] : held)
    {
        if (texture->getNumRefs() == refs)
        {
            unheld.insert(texture);
        }
    }
    if (unheld.empty())
    {
        return;
    }
    for (auto& [channel, slots] : mTypeSlots)
    {
        for (TypeSlot& slot : slots.types)
        {
            if (slot.texture.notNull() && unheld.count(slot.texture.get()) != 0)
            {
                slot = TypeSlot();
            }
        }
    }
}

void MetalEncoder::closePass(const EncoderTracker::Closing& closing, const char* call)
{
    if (!closing.warning.empty())
    {
        LL_WARNS("RHI") << call << ": " << closing.warning << LL_ENDL;
    }
    // A compute pass's encoder before anything else: at the frame's end it is the innermost.
    if (closing.endCompute)
    {
        closeCompute();
    }
    // The pass's native encoder first: it hands its debug groups back to the command buffer before its end,
    // where the pass's own are popped after it.
    if (closing.endPass)
    {
        close(true);
        mSegment = RenderPassDesc();
        mSegmentWork = false;
    }
    if (closing.resumed)
    {
        // The suspended stack pass goes on in a new segment; the tracker made its attachments load.
        mSegment = mTracker.pass();
        mSegmentWork = false;
    }
    mDevice.popGroups(closing.popInPass + closing.popOutside);
}

bool MetalEncoder::splitPass()
{
    // A compute pass's encoder ends before a submit too; its next dispatch opens another (openCompute).
    closeCompute();
    bool closed = false;
    if (mTracker.inPass())
    {
        // A pass break only when a segment really closed; a split with nothing recorded is none.
        closed = close(false);
        if (closed)
        {
            ++mStats.passBreaks;
        }
        for (const MetalCopy& copy : mPendingCopies)
        {
            mDevice.encodeCopy(copy);
        }
        mPendingCopies.clear();
    }
    return closed;
}

void MetalEncoder::breakForCopies()
{
    if (!mPendingCopies.empty())
    {
        splitPass();
    }
}

void MetalEncoder::reset()
{
    abandonQuery();
    mTracker = EncoderTracker();
    mDepthClears.clear();
    mSegment = RenderPassDesc();
    mSegmentWork = false;
    mNative = nil;
    mOpenPass = nil;
    mNativePipeline = nil;
    mDrawState = nil;
    mDrawPipelineId = 0;
    mLastPass = nil;
    mLastEncoderLabel = nil;
    mPendingCopies.clear();
    mNewTargets.clear();
    mPrograms.clear();
    mLastProgramId = 0;
    mLastProgram = nullptr;
    mTypeSlots.clear();
    mLastSamplers.clear();
    mLastChannelSamplers = {};
    mChannelVersions.fill(++mNextVersion);
    forgetResolves();
    mSamplerIDs.clear();
    syncBlocks();
    mIndexConversions.clear();
    mIndexScratch.clear();
    mTables = {};
    mShadows = {};
    mCompute = nil;
    mComputeState = nil;
    mNativeSlots = {};
}

bool MetalEncoder::scissorLimits() const
{
    const EncoderTracker::DynamicState& dynamic = mTracker.dynamicState();
    if (!dynamic.scissorEnabled)
    {
        return false;
    }
    U32 width = 0;
    U32 height = 0;
    attachmentSize(mSegment, width, height);
    const MTLScissorRect rect = scissorRect(dynamic, width, height);
    return rect.x != 0 || rect.y != 0 || rect.width != width || rect.height != height;
}

void MetalEncoder::clear(bool color, const std::array<F32, 4>& rgba, bool depth, F32 depthValue)
{
    onRecordingThread("clear");
    if (!accept("clear", mTracker.clear(color, depth)))
    {
        return;
    }
    // A readback before the clear sees the pass without it, as glReadPixels does.
    breakForCopies();
    const bool hasDepth = depth && mSegment.depth.texture.notNull();
    bool clearsDepth = false;
    U8 colorMask = COLOR_MASK_NONE;
    const MetalFacadeClear facade = metalFacadeClear();
    if (facade.ownGL)
    {
        // LLRenderTarget's clear (clear_stack_pass) stands for gl_clear's glClear, which writes what the
        // facade's masks allow: LLRender::setColorMask's and the LLGLDepthTest scopes' depth mask. The key is
        // makesOwnGL, not the pass's stack flag: an interface clear in a stack pass is GL's glClearBufferfv
        // under the bound state, as below. The facade's record does not follow a pipeline or a depth state
        // set directly on the interface inside a stack pass (GL's setPipeline writes gGL's mask beside them);
        // no caller does that today.
        clearsDepth = hasDepth && facade.depthWrite;
        if (color)
        {
            colorMask = U8((facade.color[0] ? COLOR_MASK_R : 0) | (facade.color[1] ? COLOR_MASK_G : 0) |
                           (facade.color[2] ? COLOR_MASK_B : 0) | (facade.color[3] ? COLOR_MASK_A : 0));
            color = colorMask != COLOR_MASK_NONE; // glClear writes no colour
        }
    }
    else
    {
        // GL's clear writes depth only where the depth mask allows it, and the bound state decides that.
        const DepthStencilState* bound = mTracker.depthStencilState();
        clearsDepth = hasDepth && metalWritesDepth(bound ? &bound->desc() : nullptr);
        // The color mask is the bound pipeline's, as GL's applyPipeline sets glColorMask from it.
        const Pipeline* pipeline = mTracker.pipeline();
        colorMask = color ? (pipeline ? pipeline->desc().colorMask : U8(COLOR_MASK_ALL)) : U8(COLOR_MASK_NONE);
    }
    // Also with every colour on, a scissor keeps the clear to its rectangle, as glClear.
    if (scissorLimits() || (color && mSegment.colorCount > 0 && colorMask != COLOR_MASK_ALL))
    {
        // GL's clear keeps to the scissor and the color mask; a load action clears all of an attachment.
        clearDraw(colorMask, rgba, clearsDepth, depthValue);
        return;
    }
    bool writesColor = false;
    for (U32 i = 0; color && i < mSegment.colorCount; ++i)
    {
        writesColor = writesColor || mSegment.color[i].texture.notNull();
    }
    if (!writesColor && !clearsDepth)
    {
        return; // GL's clear writes nothing here, so the pass goes on in the same segment
    }
    if (mSegmentWork || mNative)
    {
        // After a draw the pass goes on in a new segment, whose load actions the clear becomes. The
        // clear covers all of every colour attachment (no scissor, every channel) and, when it writes depth, the depth,
        // and nothing reads the segment in between (the copies went at breakForCopies above), so the segment's encoder
        // leaves them unstored.
        mDiscardColor = color && mDevice.speedKnobs().discardStores;
        mDiscardDepth = clearsDepth && mDevice.speedKnobs().discardStores;
        splitPass();
        mDiscardColor = false;
        mDiscardDepth = false;
    }
    // Nothing of the segment is recorded yet, so the clear becomes its load actions.
    if (color)
    {
        for (U32 i = 0; i < mSegment.colorCount; ++i)
        {
            if (mSegment.color[i].texture.notNull())
            {
                mSegment.color[i].load = LoadAction::Clear;
                mSegment.color[i].clear = rgba;
            }
        }
    }
    if (clearsDepth)
    {
        mSegment.depth.load = LoadAction::Clear;
        mSegment.depth.clear = depthValue;
    }
}

void MetalEncoder::clearDraw(U8 colorMask, const std::array<F32, 4>& rgba, bool depth, F32 depthValue)
{
    if (colorMask == COLOR_MASK_NONE && !depth)
    {
        return; // GL's clear writes nothing
    }
    const PassFormats formats = passFormatsOf(mSegment);
    Program* program = mDevice.clearProgram(formats);
    if (!program)
    {
        return; // createProgram wrote the WARN line; there is no MSL of the backend's own
    }
    PipelineDesc desc;
    applyPassFormats(formats, desc);
    desc.attributeMask = 0;
    desc.colorMask = colorMask; // blending stays off
    Ref<Pipeline> pipeline = mDevice.clearPipeline(*program, desc);
    // The draw waits for a pipeline state being made; nil after a WARN line. The same mask as the
    // draws, so the clear leaves alpha 1 alone where GL has no alpha.
    IdMTLRenderPipelineState state = metalPipelineState(pipeline.get(), mDevice, alphaLessAttachments(mSegment));
    if (!state || !open() || !bindForClear(*program, rgba, depthValue))
    {
        return;
    }
    // The draw covers the whole attachment, with no culling, fill and no depth bias; the scissor
    // stays the tracker's. open() set the winding (MTL4RenderCommandEncoder.h:628), and the triangle is a front face.
    // Always passes, and writes only when depth is cleared.
    applyClearState(mSegment.depth.texture.notNull()
                        ? mDevice.depthState(mDevice.getDepthStencilState({ depth, depth, CompareFunc::AlwaysPass }).get())
                        : nil);
    if (state != mNativePipeline)
    {
        [mNative setRenderPipelineState:state];
    }
    [mNative drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    ++mClearDraws;
    mSegmentWork = true;
    // The next draw sets the tracker's state again, and its pipeline, as it finds the clear's here.
    mNativePipeline = state;
}

void MetalEncoder::setNativeViewport(const MTLViewport& viewport)
{
    const MTLViewport& had = mNativeViewport;
    if (viewport.originX != had.originX || viewport.originY != had.originY || viewport.width != had.width ||
        viewport.height != had.height || viewport.znear != had.znear || viewport.zfar != had.zfar)
    {
        [mNative setViewport:viewport];
        mNativeViewport = viewport;
    }
}

void MetalEncoder::setNativeCull(MTLCullMode mode)
{
    if (mode != mNativeCull)
    {
        [mNative setCullMode:mode];
        mNativeCull = mode;
    }
}

void MetalEncoder::setNativeDepth(IdMTLDepthStencilState state)
{
    if (state != mNativeDepth)
    {
        [mNative setDepthStencilState:state];
        mNativeDepth = state;
    }
}

void MetalEncoder::setNativeScissor()
{
    // The rectangle lies within the attachment (MTL4RenderCommandEncoder.h:158-159): scissorRect clips it.
    U32 width = 0;
    U32 height = 0;
    attachmentSize(mSegment, width, height);
    const MTLScissorRect rect = scissorRect(mTracker.dynamicState(), width, height);
    const MTLScissorRect& had = mNativeScissor;
    if (rect.x != had.x || rect.y != had.y || rect.width != had.width || rect.height != had.height)
    {
        [mNative setScissorRect:rect];
        mNativeScissor = rect;
        ++mScissorSends;
    }
}

void MetalEncoder::setNativeFill(MTLTriangleFillMode mode)
{
    if (mode != mNativeFill)
    {
        [mNative setTriangleFillMode:mode];
        mNativeFill = mode;
    }
}

void MetalEncoder::setNativeBias(float units, float slope)
{
    if (units != mNativeBiasUnits || slope != mNativeBiasSlope)
    {
        [mNative setDepthBias:units slopeScale:slope clamp:0.f];
        mNativeBiasUnits = units;
        mNativeBiasSlope = slope;
        ++mStats.depthBiasChanges;
    }
}

void MetalEncoder::setNativeClip(MTLDepthClipMode mode)
{
    if (mode != mNativeClip)
    {
        [mNative setDepthClipMode:mode];
        mNativeClip = mode;
        ++mStats.depthClipSends;
        if (!mNativeClipSent)
        {
            mNativeClipSent = true;
            ++mStats.depthClipEncoders;
        }
    }
}

void MetalEncoder::applyDrawState(PrimitiveType type)
{
    U32 width = 0;
    U32 height = 0;
    attachmentSize(mSegment, width, height);
    const EncoderTracker::DynamicState& dynamic = mTracker.dynamicState();
    MTLViewport viewport = dynamic.viewport ? metalViewport(*dynamic.viewport)
                                            : MTLViewport{ 0.0, 0.0, double(width), double(height), 0.0, 1.0 };
    MetalSurface* surface = mDevice.metalSurface();
    if (isLine(type) && surface && mSegment.colorCount > 0 && mSegment.color[0].texture.get() == surface->backBuffer())
    {
        viewport.originY += LINE_TIE_SHIFT; // GL's window framebuffer's row for a line on a row edge
    }
    setNativeViewport(viewport);
    if (mSegment.depth.texture.notNull())
    {
        // Without a depth attachment the state has nothing to test or write. A refused state is unbound, as the
        // tracker does, and null is DepthStencilDesc().
        setNativeDepth(mDevice.depthState(mTracker.depthStencilState()));
    }
    const bool polygons =
        type == PrimitiveType::Triangles || type == PrimitiveType::TriangleStrip || type == PrimitiveType::TriangleFan;
    bool bias = false;
    if (polygons)
    {
        // The fill mode applies to triangles only (MTL4RenderCommandEncoder.h:173-178), culling to front and back faces
        // (:119-122); a point or line draw leaves them for the next triangle draw, which uses them.
        setNativeCull(cullMode(dynamic.cull));
        const bool lines = dynamic.fill == FillMode::Lines;
        setNativeFill(lines ? MTLTriangleFillModeLines : MTLTriangleFillModeFill);
        bias = lines ? dynamic.biasLines : dynamic.biasFill;
    }
    // glPolygonOffset's units and factor as Metal's depthBias and slopeScale, with no clamp (MTL4RenderCommandEncoder.h:
    // 129-138; Metal applies the bias to all fragments): only for polygons, with the enable for their fill mode
    // (llrhi.h:969-971), and 0 for a point or line draw, which GL's polygon offset never moves.
    setNativeBias(bias ? dynamic.biasUnits : 0.f, bias ? dynamic.biasFactor : 0.f);
    setNativeClip(dynamic.depthClamp ? MTLDepthClipModeClamp : MTLDepthClipModeClip); // MTL4RenderCommandEncoder.h:124-127
    setNativeScissor();
    setNativeVisibility(false); // the active sample query counts the draw's samples
}

void MetalEncoder::applyClearState(IdMTLDepthStencilState depth)
{
    U32 width = 0;
    U32 height = 0;
    attachmentSize(mSegment, width, height);
    setNativeViewport({ 0.0, 0.0, double(width), double(height), 0.0, 1.0 });
    setNativeCull(MTLCullModeNone);
    if (mSegment.depth.texture.notNull())
    {
        setNativeDepth(depth);
    }
    setNativeFill(MTLTriangleFillModeFill);
    setNativeBias(0.f, 0.f);
    setNativeScissor(); // the tracker's scissor limits the clear
    setNativeVisibility(true); // GL's clear adds no samples to a query
}

void MetalEncoder::setNativeVisibility(bool neutral)
{
    MetalQueryRun* run = neutral ? nullptr : mActiveRun.get();
    const bool counting = run && run->slot >= 0 && mOpenPass.visibilityResultBuffer;
    if (run && !counting && run->slot >= 0)
    {
        run->uncounted = true; // a pass without the buffer: the query has no result, so it reads as visible
    }
    if (counting)
    {
        ++run->draws; // applyDrawState runs right before the native draw, after every refusal (MetalQuery::result)
    }
    const MTLVisibilityResultMode mode = counting ? mActiveMode : MTLVisibilityResultModeDisabled;
    const NSUInteger offset = counting ? NSUInteger(run->slot) * sizeof(U64) : 0;
    if (mode != mNativeVisibility || (counting && offset != mNativeVisibilityOffset))
    {
        [mNative setVisibilityResultMode:mode offset:offset]; // MTL4RenderCommandEncoder.h:217-229
        mNativeVisibility = mode;
        mNativeVisibilityOffset = offset;
    }
}

bool MetalEncoder::open()
{
    if (mNative)
    {
        return true;
    }
    // Metal 4's pass descriptor with Metal 3's attachments. The store actions wait for
    // close(), which knows whether the pass ends with the segment (MTL4RenderCommandEncoder.h:229-244).
    RenderPassDesc& pass = mSegment;
    if (!mDepthClears.empty())
    {
        takeDepthClear(pass);
    }
    MTL4RenderPassDescriptor* descriptor = [MTL4RenderPassDescriptor new];
    const U32 alphaLess = alphaLessAttachments(pass);
    mNativeAlphaLess = alphaLess; // setNativePipeline's mask for the segment's draws
    for (U32 i = 0; i < pass.colorCount; ++i)
    {
        ColorAttachment& color = pass.color[i];
        if (color.texture.isNull())
        {
            continue;
        }
        if (alphaLess & (1u << i))
        {
            // GL's RGB of a level and layer nothing has written is undefined, its alpha 1; Metal's
            // memory is not, so the first load of one is a clear. A clear writes alpha 1 there.
            if (color.load == LoadAction::Load && !metalWritten(color.texture.get(), color.level, color.layer))
            {
                color.load = LoadAction::Clear;
                color.clear = { 0.f, 0.f, 0.f, 1.f };
                ++mFirstLoadClears;
            }
            else if (color.load == LoadAction::DontCare)
            {
                // DontCare leaves GL's RGB as it was and its alpha 1. Metal's begins the pass
                // without the stored texels, and no draw writes the alpha here (the pipeline's alpha-less variant), so
                // the alpha would be whatever the attachment's memory held: the pass clears to (0, 0, 0, 1) instead.
                // No caller uses DontCare today.
                color.load = LoadAction::Clear;
                color.clear = { 0.f, 0.f, 0.f, 1.f };
            }
            color.clear[3] = 1.f;
        }
        MTLRenderPassColorAttachmentDescriptor* attachment = descriptor.colorAttachments[i];
        attachment.texture = metalTexture(color.texture.get());
        attachment.level = color.level;
        attachment.slice = color.layer;
        attachment.loadAction = memoryless(attachment.texture) && color.load == LoadAction::Load ? MTLLoadActionClear
                                                                                                 : loadAction(color.load);
        attachment.storeAction = MTLStoreActionUnknown;
        attachment.clearColor = MTLClearColorMake(color.clear[0], color.clear[1], color.clear[2], color.clear[3]);
    }
    if (pass.depth.texture.notNull())
    {
        MTLRenderPassDepthAttachmentDescriptor* attachment = descriptor.depthAttachment;
        attachment.texture = metalTexture(pass.depth.texture.get());
        attachment.level = pass.depth.level;
        attachment.slice = pass.depth.layer;
        attachment.loadAction = memoryless(attachment.texture) && pass.depth.load == LoadAction::Load
                                    ? MTLLoadActionClear
                                    : loadAction(pass.depth.load);
        attachment.storeAction = MTLStoreActionUnknown;
        attachment.clearDepth = pass.depth.clear;
    }

    // Once a sample query has made it, every pass carries the visibility result buffer, accumulating across
    // encoders (MTL4RenderPass.h:96-100; Reset would zero it at each encoder, MTLRenderPass.h:57-64), so a query spans
    // the backend's pass breaks and GL's framebuffer changes; each begin zeroes its own slot.
    if (IdMTLBuffer visibility = mDevice.visibilityBuffer())
    {
        descriptor.visibilityResultBuffer = visibility;
        descriptor.visibilityResultType = MTLVisibilityResultTypeAccumulate;
    }

    zeroNewTargets(); // before the segment, which may draw into or sample one
    mDevice.endCopies();
    IdMTL4CommandBuffer buffer = mDevice.commandBuffer();
    // The open debug groups leave the command buffer before the encoder begins and go on in the encoder
    // (MTL4CommandBuffer.h:157-160, MTL4CommandEncoder.h:161-164), so no native group crosses its begin or end.
    mDevice.endGroups();
    IdMTL4RenderCommandEncoder encoder = [buffer renderCommandEncoderWithDescriptor:descriptor];
    mDevice.beginGroups(encoder); // on the command buffer again when Metal gave no encoder
    if (!encoder)
    {
        LL_WARNS("RHI") << "Metal gave no render encoder for pass '" << pass.name << "'" << LL_ENDL;
        return false;
    }
    // The segment's encoder stores its attachments, so a later segment's Load is not a first one.
    for (U32 i = 0; i < pass.colorCount; ++i)
    {
        if (pass.color[i].texture.notNull())
        {
            metalMarkWritten(pass.color[i].texture.get(), pass.color[i].level, pass.color[i].layer);
        }
    }
    encoder.label = metalString(pass.name);
    mLastEncoderLabel = encoder.label;
    ++mRenderEncoders;
    // The pass waits for the queue's earlier work and sees what it wrote; with split barriers its vertex
    // work does not, until a draw's vertex stage samples (setNativePipeline).
    mVertexWaitOpen = mDevice.passBarrier(encoder);
    // The pass timer's begin, after the barrier.
    mNativeStamp = mDevice.passTimes().recording()
                       ? mDevice.passTimes().beginRender(encoder, pass, mDevice.lifecycle().currentFrame())
                       : MetalPassStamp();
    // The dynamic state waits for the first draw or clear draw (applyDrawState, applyClearState), as
    // the scissor does, so validation never sees a state that no draw used. The copies start at Metal's
    // defaults: the viewport and the scissor are the whole attachment (Apple's documentation of
    // MTLRenderCommandEncoder's setViewport(_:) and setScissorRect(_:); MTL4RenderCommandEncoder.h:154-160 names no
    // default, only that the rectangle lies within the attachment); no culling, fill, no depth bias and clip are the
    // enums' zero values and the documented defaults of setCullMode(_:), setTriangleFillMode(_:), setDepthBias(_:
    // slopeScale:clamp:) and setDepthClipMode(_:) (MTLRenderCommandEncoder.h:52-70); no depth-stencil state.
    U32 width = 0;
    U32 height = 0;
    attachmentSize(pass, width, height);
    mNativeViewport = { 0.0, 0.0, double(width), double(height), 0.0, 1.0 };
    mNativeScissor = { 0, 0, width, height };
    mNativeCull = MTLCullModeNone;
    mNativeDepth = nil;
    mNativeFill = MTLTriangleFillModeFill;
    mNativeBiasUnits = 0.f;
    mNativeBiasSlope = 0.f;
    mNativeClip = MTLDepthClipModeClip;
    mNativeClipSent = false;
    mNativeVisibility = MTLVisibilityResultModeDisabled; // Metal's default, MTLRenderCommandEncoder.h:38-42
    mNativeVisibilityOffset = 0;
    // The vertex stage flips y, which turns GL's counter-clockwise front faces
    // clockwise (MTL4RenderCommandEncoder.h:624-628).
    [encoder setFrontFacingWinding:MTLWindingClockwise];
    if (mTables[0])
    {
        // Metal snapshots the tables at each draw (MTL4RenderCommandEncoder.h:613-622).
        [encoder setArgumentTable:mTables[0] atStages:MTLRenderStageVertex];
        [encoder setArgumentTable:mTables[1] atStages:MTLRenderStageFragment];
    }
    mNative = encoder;
    mOpenPass = descriptor;
    mNativePipeline = nil;
    mDevice.noteGroupsWhileEncoding();

    // A segment after this one continues from what this one stores.
    for (U32 i = 0; i < pass.colorCount; ++i)
    {
        pass.color[i].load = LoadAction::Load;
    }
    pass.depth.load = LoadAction::Load;
    return true;
}

bool MetalEncoder::close(bool last)
{
    if (!mNative)
    {
        // Only an open encoder leaves stores out; a segment begun here only for its clears stores them.
        mDiscardColor = false;
        mDiscardDepth = false;
        const RenderPassDesc& pass = mSegment;
        bool clears = pass.depth.texture.notNull() && pass.depth.load == LoadAction::Clear;
        for (U32 i = 0; i < pass.colorCount; ++i)
        {
            clears = clears || (pass.color[i].texture.notNull() && pass.color[i].load == LoadAction::Clear);
        }
        // A segment that only loads and records nothing changes nothing; one that clears needs its
        // encoder for the load actions, unless a later segment takes its depth clear.
        if (!clears || carryDepthClear(last) || !open())
        {
            return false;
        }
    }
    llassert(!last || (!mDiscardColor && !mDiscardDepth)); // a pass's last segment keeps its own store actions
    setStoreActions(last, false);
    mDiscardColor = false;
    mDiscardDepth = false;
    endNative();
    return true;
}

bool MetalEncoder::carryDepthClear(bool last)
{
    RenderPassDesc& pass = mSegment;
    const DepthAttachment& depth = pass.depth;
    if (depth.texture.isNull() || depth.load != LoadAction::Clear || (last && depth.store != StoreAction::Store)
        || (depth.texture->desc().usage & (TEXTURE_SAMPLED | TEXTURE_STORAGE)) != 0
        || memoryless(metalTexture(depth.texture.get())))
    {
        return false;
    }
    for (U32 i = 0; i < pass.colorCount; ++i)
    {
        if (pass.color[i].texture.notNull() && pass.color[i].load != LoadAction::Load)
        {
            return false;
        }
    }
    // A later clear of the same depth level and layer takes the place of one still waiting.
    const auto same = std::find_if(mDepthClears.begin(), mDepthClears.end(), [&depth](const DepthClear& waiting)
    { return waiting.texture == depth.texture && waiting.level == depth.level && waiting.layer == depth.layer; });
    DepthClear& clear = same != mDepthClears.end() ? *same : mDepthClears.emplace_back();
    clear.texture = depth.texture;
    clear.level = depth.level;
    clear.layer = depth.layer;
    clear.value = depth.clear;
    clear.name = pass.name;
    // A segment after this one in the same pass loads the depth, and takes the clear at its open().
    pass.depth.load = LoadAction::Load;
    return true;
}

void MetalEncoder::takeDepthClear(RenderPassDesc& pass)
{
    DepthAttachment& depth = pass.depth;
    if (depth.texture.isNull())
    {
        return;
    }
    const auto waiting = std::find_if(mDepthClears.begin(), mDepthClears.end(), [&depth](const DepthClear& clear)
    { return clear.texture == depth.texture && clear.level == depth.level && clear.layer == depth.layer; });
    if (waiting == mDepthClears.end())
    {
        return;
    }
    if (depth.load == LoadAction::Load)
    {
        depth.load = LoadAction::Clear;
        depth.clear = waiting->value;
        ++mDepthClearsCarried;
    }
    mDepthClears.erase(waiting);
}

void MetalEncoder::settleDepthClears(const Texture* texture)
{
    // A depth clear that is still the load action of the open segment, which has no encoder yet, comes
    // before the call too: the split carries it into mDepthClears (or encodes it, when carryDepthClear refuses it),
    // so the copy, upload or readback that follows does not run ahead of a clear GL ordered before it.
    const DepthAttachment& segmentDepth = mSegment.depth;
    if (mTracker.inPass() && !mNative && segmentDepth.texture.notNull() && segmentDepth.load == LoadAction::Clear
        && (!texture || segmentDepth.texture.get() == texture))
    {
        splitPass();
    }
    if (mDepthClears.empty())
    {
        return;
    }
    const auto due = [texture](const DepthClear& clear) { return !texture || clear.texture.get() == texture; };
    if (std::none_of(mDepthClears.begin(), mDepthClears.end(), due))
    {
        return;
    }
    // No render or compute encoder may be open beside the clear's own.
    splitPass();
    std::vector<DepthClear> waiting;
    waiting.swap(mDepthClears);
    for (DepthClear& clear : waiting)
    {
        if (!due(clear))
        {
            mDepthClears.push_back(std::move(clear));
            continue;
        }
        MTL4RenderPassDescriptor* pass = [MTL4RenderPassDescriptor new];
        pass.depthAttachment.texture = metalTexture(clear.texture.get());
        pass.depthAttachment.level = clear.level;
        pass.depthAttachment.slice = clear.layer;
        pass.depthAttachment.loadAction = MTLLoadActionClear;
        pass.depthAttachment.storeAction = MTLStoreActionStore;
        pass.depthAttachment.clearDepth = clear.value;
        IdMTL4RenderCommandEncoder encoder = openOwnEncoder(pass, metalString(clear.name));
        if (!encoder)
        {
            LL_WARNS("RHI") << "Metal gave no render encoder for the depth clear of pass '" << clear.name << "'"
                            << LL_ENDL;
            continue;
        }
        closeOwnEncoder(encoder);
        ++mDepthClearsEncoded;
    }
}

void MetalEncoder::closeForTeardown()
{
    // Only an encoder that is open; a segment that only clears is not begun now. Nobody
    // reads the attachments of a command buffer that ends unsent, so they are not stored.
    if (mNative)
    {
        setStoreActions(false, true);
        endNative();
    }
}

void MetalEncoder::setStoreActions(bool last, bool teardown)
{
    // A segment that is not the pass's last stores color and depth for the next, whatever the
    // pass's store actions. The deferred setters take no Unknown (MTL4RenderCommandEncoder.h:229-244).
    const RenderPassDesc& pass = mSegment;
    for (U32 i = 0; i < pass.colorCount; ++i)
    {
        if (pass.color[i].texture.notNull())
        {
            const bool discard = !teardown && !last && mDiscardColor; // clear()
            const MTLStoreAction action = teardown || discard || memoryless(metalTexture(pass.color[i].texture.get()))
                                              ? MTLStoreActionDontCare
                                          : last ? storeAction(pass.color[i].store)
                                                 : MTLStoreActionStore;
            mDiscardedStores += discard ? 1 : 0;
            [mNative setColorStoreAction:action atIndex:i];
            mOpenPass.colorAttachments[i].storeAction = action;
        }
    }
    if (pass.depth.texture.notNull())
    {
        const bool discard = !teardown && !last && mDiscardDepth; // clear()
        const MTLStoreAction action = teardown || discard || memoryless(metalTexture(pass.depth.texture.get()))
                                          ? MTLStoreActionDontCare
                                      : last ? storeAction(pass.depth.store)
                                             : MTLStoreActionStore;
        mDiscardedStores += discard ? 1 : 0;
        [mNative setDepthStoreAction:action];
        mOpenPass.depthAttachment.storeAction = action;
    }
}

void MetalEncoder::endNative()
{
    if (mNativeStamp.frame)
    {
        mDevice.passTimes().endRender(mNative, mNativeStamp);
        mNativeStamp = MetalPassStamp();
    }
    // The encoder's debug groups are popped before its end and go on in the command buffer.
    mDevice.endGroups();
    [mNative endEncoding];
    mDevice.beginGroups(nil);
    mLastPass = mOpenPass;
    mNative = nil;
    mOpenPass = nil;
    mNativePipeline = nil;
    mVertexWaitOpen = false;
    mSegmentWork = false;
}

IdMTL4RenderCommandEncoder MetalEncoder::openOwnEncoder(MTL4RenderPassDescriptor* pass, NSString* label)
{
    zeroNewTargets(); // before the encoder, which may draw into one; finds none when it is zeroNewTargets' own
    mDevice.endCopies();
    // As open(): the command buffer first, because a new one begins with the open debug groups pushed; then the
    // groups leave it before the encoder begins and go on in the encoder (MTL4CommandBuffer.h:157-160,
    // MTL4CommandEncoder.h:161-164), so no native group crosses its begin or end.
    IdMTL4CommandBuffer buffer = mDevice.commandBuffer();
    mDevice.endGroups();
    IdMTL4RenderCommandEncoder encoder = [buffer renderCommandEncoderWithDescriptor:pass];
    mDevice.beginGroups(encoder); // on the command buffer again when Metal gave no encoder
    mDevice.noteGroupsWhileEncoding(encoder != nil);
    if (!encoder)
    {
        return nil;
    }
    encoder.label = label;
    ++mRenderEncoders;
    // The encoder sees what the queue's earlier work wrote (MTL4CommandEncoder.h:50-58).
    mDevice.queueBarrier(encoder);
    mOwnStamp = mDevice.passTimes().recording()
                    ? mDevice.passTimes().beginOwnRender(encoder, label, mDevice.lifecycle().currentFrame())
                    : MetalPassStamp();
    return encoder;
}

void MetalEncoder::closeOwnEncoder(IdMTL4RenderCommandEncoder encoder)
{
    if (mOwnStamp.frame)
    {
        mDevice.passTimes().endRender(encoder, mOwnStamp);
        mOwnStamp = MetalPassStamp();
    }
    // The encoder's debug groups are popped before its end and go on in the command buffer.
    mDevice.endGroups();
    [encoder endEncoding];
    mDevice.beginGroups(nil);
}

namespace
{
    /// Bytes of one texel of an uncompressed color format, for copies from a buffer of zeros; 0 for the others.
    U32 zeroTexelBytes(Format format)
    {
        switch (format)
        {
        case Format::R8Unorm:
        case Format::R8Uint:          return 1;
        case Format::RG8Unorm:
        case Format::R16Float:
        case Format::R16Uint:         return 2;
        case Format::RGBA8Unorm:
        case Format::RGBA8Unorm_sRGB:
        case Format::BGRA8Unorm:
        case Format::RG16Float:
        case Format::R32Float:
        case Format::R32Uint:
        case Format::RGB10A2Unorm:
        case Format::RG11B10Float:    return 4;
        case Format::RGBA16Float:
        case Format::RGBA16Unorm:
        case Format::RG32Float:       return 8;
        case Format::RGBA32Float:     return 16;
        default:                      return 0;
        }
    }

    /// A level's layers: one, a cube's six faces, or a cube array's six per cube.
    U32 levelLayers(const TextureDesc& desc)
    {
        return desc.type == TextureType::Tex2D ? 1 : desc.type == TextureType::Cube ? 6 : desc.layers * 6;
    }
}

void MetalEncoder::zeroLater(Texture* texture)
{
    if (!texture || std::this_thread::get_id() != mRecordingThread)
    {
        return;
    }
    const TextureDesc& desc = texture->desc();
    // Cubes and cube arrays only: a 3D texture has one slice, its layers being z, and every one is uploaded
    // (TextureType::Tex3D); levelLayers counts six per layer, which a copy into slice 1 and on would exceed.
    if ((desc.type == TextureType::Cube || desc.type == TextureType::CubeArray) && zeroTexelBytes(desc.format) != 0)
    {
        mNewTargets.emplace_back(texture);
    }
}

void MetalEncoder::zeroNewTargets()
{
    if (mNewTargets.empty())
    {
        return;
    }
    std::vector<Ref<Texture>> targets;
    targets.swap(mNewTargets); // the encoders opened below find none
    // The cubes that are render targets first, a level in a layered pass of its own (renderTargetArrayLength,
    // MTL4RenderPass.h:42), with no draw; then the others, every layer of a level copied from one buffer of zeros, on
    // one encoder of copies. Each cleared layer is written.
    IdMTL4ComputeCommandEncoder copies = nil;
    for (const bool renderTargets : { true, false })
    {
        for (const Ref<Texture>& texture : targets)
        {
            const TextureDesc& desc = texture->desc();
            if (texture->getNumRefs() == 1 || ((desc.usage & TEXTURE_RENDER_TARGET) != 0) != renderTargets)
            {
                continue; // only this list holds it, or its turn is the other round
            }
            const U32 layers = levelLayers(desc);
            for (U32 level = 0; level < desc.mipLevels; ++level)
            {
                std::vector<U32> unwritten;
                for (U32 layer = 0; layer < layers; ++layer)
                {
                    if (!metalWritten(texture.get(), level, layer))
                    {
                        unwritten.push_back(layer);
                    }
                }
                if (unwritten.empty() || (renderTargets && unwritten.size() != layers))
                {
                    continue; // a layered pass would clear what a write has reached
                }
                if (renderTargets)
                {
                    MTL4RenderPassDescriptor* pass = [MTL4RenderPassDescriptor new];
                    pass.colorAttachments[0].texture = metalTexture(texture.get());
                    pass.colorAttachments[0].level = level;
                    pass.colorAttachments[0].slice = 0;
                    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
                    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
                    pass.colorAttachments[0].clearColor =
                        MTLClearColorMake(0.0, 0.0, 0.0, hasAlphaInGLFormat(desc.glFormat) ? 0.0 : 1.0);
                    if (layers > 1)
                    {
                        pass.renderTargetArrayLength = layers;
                    }
                    IdMTL4RenderCommandEncoder encoder = openOwnEncoder(pass, @"Clear new target");
                    if (!encoder)
                    {
                        LL_WARNS("RHI") << "Metal gave no render encoder to clear level " << level << " of new target '"
                                        << texture->debugName() << "'" << LL_ENDL;
                        continue;
                    }
                    closeOwnEncoder(encoder);
                }
                else
                {
                    // Zero bytes are 0 in every uncompressed format; a texture that is no render target reads alpha 1
                    // through its swizzle where its GL format keeps none (createMetalTexture).
                    const U32 width = std::max(1u, desc.width >> level);
                    const U32 height = std::max(1u, desc.height >> level);
                    const U64 rowBytes = U64(width) * zeroTexelBytes(desc.format);
                    const MetalAllocation zeros = mDevice.frameAllocation(rowBytes * height);
                    if (!zeros.buffer)
                    {
                        continue; // frameAllocation wrote the WARN line
                    }
                    std::memset(zeros.cpu, 0, size_t(rowBytes * height));
                    if (!copies && !(copies = mDevice.beginUploadCopies()))
                    {
                        return; // beginUploadCopies wrote the WARN line
                    }
                    for (const U32 layer : unwritten)
                    {
                        [copies copyFromBuffer:zeros.buffer
                                  sourceOffset:zeros.offset
                             sourceBytesPerRow:NSUInteger(rowBytes)
                           sourceBytesPerImage:NSUInteger(rowBytes * height)
                                    sourceSize:MTLSizeMake(width, height, 1)
                                     toTexture:metalTexture(texture.get())
                              destinationSlice:layer
                              destinationLevel:level
                             destinationOrigin:MTLOriginMake(0, 0, 0)
                                       options:MTLBlitOptionNone];
                    }
                }
                for (const U32 layer : unwritten)
                {
                    metalMarkWritten(texture.get(), level, layer);
                }
                ++mNewTargetClears;
            }
        }
    }
    if (copies)
    {
        mDevice.endCopies();
    }
}

// Recorded only (the scissor too): the next draw or clear draw gives the native encoder what it
// uses (applyDrawState, applyClearState), so a state set and set again before a draw never reaches Metal ("Previous
// setCullMode was unused", 32k validation warnings in a first world run).

void MetalEncoder::setDepthStencilState(DepthStencilState* state)
{
    onRecordingThread("setDepthStencilState");
    accept("setDepthStencilState", mTracker.setDepthStencilState(state));
}

void MetalEncoder::setViewport(S32 x, S32 y, S32 width, S32 height)
{
    onRecordingThread("setViewport");
    accept("setViewport", mTracker.setViewport(x, y, width, height));
}

void MetalEncoder::setScissor(bool enabled, S32 x, S32 y, S32 width, S32 height)
{
    onRecordingThread("setScissor");
    accept("setScissor", mTracker.setScissor(enabled, x, y, width, height));
}

void MetalEncoder::setCullMode(CullMode mode)
{
    onRecordingThread("setCullMode");
    accept("setCullMode", mTracker.setCullMode(mode));
}

void MetalEncoder::setFillMode(FillMode mode)
{
    onRecordingThread("setFillMode");
    accept("setFillMode", mTracker.setFillMode(mode));
}

void MetalEncoder::setDepthBias(F32 factor, F32 units, bool enableFill, bool enableLines)
{
    onRecordingThread("setDepthBias");
    accept("setDepthBias", mTracker.setDepthBias(factor, units, enableFill, enableLines));
}

void MetalEncoder::setDepthClamp(bool enabled)
{
    onRecordingThread("setDepthClamp");
    // Metal's own depth clamp, setDepthClipMode Clamp, at the next draw.
    accept("setDepthClamp", mTracker.setDepthClamp(enabled));
}

void MetalEncoder::setPointSize(F32 size)
{
    onRecordingThread("setPointSize");
    // The tracker keeps it; a point draw writes it to the variant's point_size.
    accept("setPointSize", mTracker.setPointSize(size));
}

void MetalEncoder::setLineWidth(F32 width)
{
    onRecordingThread("setLineWidth");
    // Lines are 1 pixel on Mac; the tracker clamps to lineWidthRange (1-1), no WARN.
    accept("setLineWidth", mTracker.setLineWidth(width));
}

void MetalEncoder::pushDebugGroup(std::string_view name)
{
    onRecordingThread("pushDebugGroup");
    if (accept("pushDebugGroup", mTracker.pushDebugGroup()))
    {
        mDevice.pushGroup(name);
    }
}

void MetalEncoder::popDebugGroup()
{
    onRecordingThread("popDebugGroup");
    if (accept("popDebugGroup", mTracker.popDebugGroup()))
    {
        mDevice.popGroups(1);
    }
}

void MetalEncoder::waitForFence(Fence* fence)
{
    onRecordingThread("waitForFence");
    // llrhi.h's waitForFence, keyed on Metal's own fence type as GL keys on its: a null fence, or one of another
    // backend, guards no work of this one and is no wait; another backend's is not called into.
    if (!fence)
    {
        return;
    }
    MetalUploadFence* uploads = dynamic_cast<MetalUploadFence*>(fence);
    if (!uploads)
    {
        LL_WARNS("RHI") << "waitForFence: the fence is not the Metal upload context's; no wait" << LL_ENDL;
        return;
    }
    if (uploads->signaled())
    {
        ++uploads->state().signalledAtHandover;
    }
    else
    {
        // The command buffer being recorded, and every later one, runs after the uploads; the CPU does not wait.
        mDevice.waitForUploads(uploads->event(), uploads->value());
        ++uploads->state().queueWaits;
    }
    // The mips the upload thread asked for, made here after the queue's wait with the filter
    // the main thread's own mips use, so a texture gets the same mips with or without the thread; once.
    for (const Ref<Texture>& texture : uploads->takeMips())
    {
        generateMipmaps(texture.get());
    }
}

// Bindings: recorded now; bindForDraw puts them in the argument tables at each draw.

void MetalEncoder::setPipeline(Pipeline* pipeline)
{
    onRecordingThread("setPipeline");
    // The native encoder gets the state at the draw that uses it, when it changes.
    accept("setPipeline", mTracker.setPipeline(pipeline));
}

void MetalEncoder::setVertexBuffer(Buffer* buffer, const VertexStreams& streams)
{
    onRecordingThread("setVertexBuffer");
    accept("setVertexBuffer", mTracker.setVertexBuffer(buffer, streams));
}

void MetalEncoder::setTexture(U32 channel, Texture* texture, const SamplerDesc& sampling)
{
    onRecordingThread("setTexture");
    // The sampler is chosen at the draw from the sampling; a refused texture is unbound.
    // The texture goes into the channel's slot of its type, and a null tell empties the slot of the type the
    // channel got last, as LLTexUnit::unbind(type) clears only the current target; a refused texture empties its type's.
    const bool bound = accept("setTexture", mTracker.setTexture(channel, texture));
    if (bound && texture && !mDepthClears.empty())
    {
        settleDepthClears(texture);
    }
    if (bound && texture)
    {
        ChannelSlots& slots = mTypeSlots[channel];
        const TextureType type = texture->desc().type;
        TypeSlot& slot = slots.types[size_t(type)];
        // The texture and sampling the channel's last type's slot holds already: the channel's slots and its binding
        // stay as they are, and so does its version.
        if (slots.last == type && slot.texture.get() == texture && slot.sampling == sampling)
        {
            return;
        }
        slots.last = type;
        slot = { texture, sampling };
    }
    else if (const auto found = mTypeSlots.find(channel); found != mTypeSlots.end())
    {
        found->second.last = texture ? texture->desc().type : found->second.last;
        found->second.types[size_t(found->second.last)] = TypeSlot();
    }
    // The channel's places resolve again at their next draw.
    if (channel < RESOLVE_CHANNELS)
    {
        mChannelVersions[channel] = ++mNextVersion;
    }
}

bool MetalEncoder::samplerID(const SamplerDesc& desc, MTLResourceID& id)
{
    if (const auto found = mSamplerIDs.find(desc); found != mSamplerIDs.end())
    {
        id = found->second.id;
        return true;
    }
    Ref<Sampler> sampler = mDevice.getSampler(desc);
    if (sampler.isNull())
    {
        return false; // getSampler wrote the WARN line
    }
    id = metalSamplerID(sampler.get());
    mSamplerIDs.emplace(desc, CachedSampler{ sampler, id });
    return true;
}

void MetalEncoder::forgetResolves()
{
    for (auto& stage : mResolved)
    {
        for (ResolvedPlace& place : stage)
        {
            place.version = 0;
        }
    }
}

void MetalEncoder::syncBlocks()
{
    const auto& tracked = mTracker.uniformBuffers();
    for (U32 slot = 0; slot < mBlocks.size(); ++slot)
    {
        const auto bound = tracked.find(slot);
        mBlocks[slot].buffer = bound != tracked.end() ? bound->second.get() : nullptr;
    }
}

// Uniforms -------------------------------------------------------------------------------------

MetalProgramState& MetalEncoder::programState(U64 programId, const MetalProgramLayout& layout)
{
    // The map's elements keep their place when it grows; forgetProgram and reset() let go of the last answer.
    if (mLastProgram && programId == mLastProgramId)
    {
        return *mLastProgram;
    }
    const auto [found, made] = mPrograms.try_emplace(programId);
    if (made)
    {
        // GL's defaults: every uniform 0, every sampler uniform on channel 0 (channels' default).
        found->second.blocks[0].assign(layout.vertex.blockSize, std::byte(0));
        found->second.blocks[1].assign(layout.fragment.blockSize, std::byte(0));
    }
    mLastProgramId = programId;
    mLastProgram = &found->second;
    return found->second;
}

std::span<const std::byte> MetalEncoder::uniformBlock(Program* program, ShaderStage stage) const
{
    const auto found = program ? mPrograms.find(program->id()) : mPrograms.end();
    if (found == mPrograms.end())
    {
        return {};
    }
    return found->second.blocks[stage == ShaderStage::Fragment ? 1 : 0];
}

void MetalEncoder::setUniform(Program* program, U32 uniformIndex, std::span<const std::byte> value, U32 count)
{
    onRecordingThread("setUniform");
    if (!accept("setUniform", mTracker.setUniform(program, uniformIndex, value.size(), count)))
    {
        return;
    }
    // With checks off the tracker lets through what does not fit, so nothing is written outside
    // the uniform's place.
    const MetalProgramLayout* layout = metalProgramLayout(program);
    if (!layout || uniformIndex >= program->reflection().uniforms.size() || count == 0 || value.size() % count)
    {
        return;
    }
    const UniformInfo& uniform = program->reflection().uniforms[uniformIndex];
    const size_t elements = std::min<size_t>(count, uniform.arraySize);
    const size_t element = value.size() / count;
    const size_t columns = matrixColumns(uniform.type);
    const size_t column = element / columns;
    MetalProgramState& state = programState(program->id(), *layout);
    const MetalStageLayout* stages[2] = { &layout->vertex, &layout->fragment };
    for (size_t s = 0; s < 2; ++s)
    {
        if (uniformIndex >= stages[s]->places.size() || stages[s]->places[uniformIndex].size == 0)
        {
            continue; // the stage does not use it
        }
        const MetalUniformPlace& place = stages[s]->places[uniformIndex];
        std::vector<std::byte>& block = state.blocks[s];
        const size_t end = std::min<size_t>(block.size(), size_t(place.offset) + place.size);
        // Only bytes that differ mark the stage, so its block goes to the ring again.
        bool changed = false;
        const auto put = [&block, &changed, end](size_t at, const std::byte* bytes, size_t size)
        {
            if (at + size <= end && std::memcmp(block.data() + at, bytes, size) != 0)
            {
                std::memcpy(block.data() + at, bytes, size);
                changed = true;
            }
        };
        if ((elements == 1 || place.arrayStride == element) && (columns == 1 || place.matrixStride == column))
        {
            // The block's layout is the value's: one copy.
            put(place.offset, value.data(), elements * element);
        }
        else
        {
            // The value is tight and column-major; in the block each element is at offset + i *
            // arrayStride and each matrix column at offset + c * matrixStride, and the padding stays.
            for (size_t i = 0; i < elements; ++i)
            {
                for (size_t c = 0; c < columns; ++c)
                {
                    put(place.offset + i * place.arrayStride + c * place.matrixStride,
                        value.data() + i * element + c * column, column);
                }
            }
        }
        state.dirty[s] = state.dirty[s] || changed;
    }
}

void MetalEncoder::setSamplerChannel(Program* program, U32 uniformIndex, U32 channel)
{
    onRecordingThread("setSamplerChannel");
    const MetalProgramLayout* layout = metalProgramLayout(program);
    if (!accept("setSamplerChannel", mTracker.setSamplerChannel(program, uniformIndex)) || !layout)
    {
        return;
    }
    std::vector<U32>& channels = programState(program->id(), *layout).channels;
    if (uniformIndex >= channels.size())
    {
        channels.resize(uniformIndex + 1, 0);
    }
    channels[uniformIndex] = channel;
}

// Draws ----------------------------------------------------------------------------------

void MetalEncoder::draw(PrimitiveType type, U32 firstVertex, U32 vertexCount)
{
    onRecordingThread("draw");
    // A draw with a pipeline still being made waits for it (llrhi.h:790-791), so the tracker's
    // refusal names one that failed meanwhile.
    waitForPipeline();
    if (!accept("draw", mTracker.draw(type)))
    {
        return;
    }
    const std::optional<MTLPrimitiveType> primitive = nativePrimitive(type);
    // A fan, strip or loop draws its cached list from firstVertex.
    const MetalIndexList list = primitive ? MetalIndexList() : mDevice.generatedIndices(type, vertexCount);
    if (vertexCount == 0 || (!primitive && !list.buffer))
    {
        return; // nothing to draw, or generatedIndices wrote the WARN line
    }
    // A readback recorded in the pass reads it without this draw.
    breakForCopies();
    // With the Metal debug setting the generated list is resident, as what bindForDraw binds.
    if (!primitive && mDevice.debugChecks() && !mDevice.residency().contains(list.buffer))
    {
        LL_WARNS("RHI") << "draw: index list of " << vertexCount << " vertices is not resident" << LL_ENDL;
        return;
    }
    if (!bindForDraw("draw", firstVertex, vertexCount, false) || !open())
    {
        return;
    }
    if (!setNativePipeline())
    {
        return;
    }
    applyDrawState(type);
    if (primitive)
    {
        [mNative drawPrimitives:*primitive vertexStart:firstVertex vertexCount:vertexCount];
    }
    else
    {
        // baseVertex moves the list's 0 to firstVertex (MTL4RenderCommandEncoder.h:418-426).
        const U32 size = indexSize(list.type);
        [mNative drawIndexedPrimitives:metalListPrimitive(type)
                            indexCount:list.count
                             indexType:list.type == IndexType::U16 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32
                           indexBuffer:list.buffer.gpuAddress
                     indexBufferLength:U64(list.count) * size
                         instanceCount:1
                            baseVertex:firstVertex
                          baseInstance:0];
    }
    ++mNativeDraws;
    mTracker.drawMade(); // the draw reached the native encoder
    mSegmentWork = true;
    if (mTracker.pipeline() && mTracker.pipeline()->native())
    {
        markNativeWrites(NativeStage::Vertex);
        markNativeWrites(NativeStage::Fragment);
    }
}

void MetalEncoder::drawIndexed(PrimitiveType type, U32 firstIndex, U32 indexCount)
{
    onRecordingThread("drawIndexed");
    // As draw: the tracker's refusal names a pipeline that failed while the draw waited.
    waitForPipeline();
    if (!accept("drawIndexed", mTracker.drawIndexed(type, firstIndex, indexCount)))
    {
        return;
    }
    const std::optional<MTLPrimitiveType> primitive = nativePrimitive(type);
    // A fan, strip or loop is its list of the indices first..first + count, in the ring.
    const U32 listCount = primitive ? indexCount : metalListCount(type, indexCount);
    if (indexCount == 0 || listCount == 0)
    {
        return;
    }
    // The tracker's checks, always on here, refused a draw without an index buffer or past its end
    // (llrhi.cpp, EncoderTracker::drawIndexed). The address is the memory current at the draw, a
    // dynamic buffer's too, and the length a whole number of indices from the first; both keep
    // Metal's alignment to the index size (MTL4RenderCommandEncoder.h:322-345).
    Buffer* indices = mTracker.indexBuffer();
    const IndexType indexType = mTracker.indexType();
    const U64 size = indexSize(indexType);
    const U64 skip = U64(firstIndex) * size;
    MTLGPUAddress address = metalAddress(indices) + skip;
    U64 length = ((indices->desc().size - skip) / size) * size;
    U32 count = indexCount;
    // Metal restarts a strip at a 16-bit 0xFFFF, GL 4.1 draws vertex 65535 (GL_PRIMITIVE_RESTART is
    // off, indra never enables it). golden level 1's index_65535 showed the restart in the native LineStrip and the
    // LineLoop's ring copy drawn as a line strip, not in lists (Triangles, Lines, the generated lists of fans and
    // strips). Such a draw goes through the ring widened to 32 bits, only when its buffer was given a 0xFFFF where its
    // indices are written and the binding can reach vertex 65535 (its real vertex count); every other draw as it is.
    const MTLPrimitiveType drawn = primitive ? *primitive : metalListPrimitive(type);
    const bool widen = indexType == IndexType::U16
                       && (drawn == MTLPrimitiveTypeLineStrip || drawn == MTLPrimitiveTypeTriangleStrip)
                       && metalIndexHoldsRestart(indices)
                       && metalWholeVertices(mTracker.vertexBuffer(), mTracker.streams(),
                                             metalPipelineAttributes(mTracker.pipeline())) > 0xFFFF;
    // The TriangleStrip arm does not hold today, as a TriangleStrip is always drawn as a generated list
    // (metalListPrimitive); it keeps a future native strip right.
    const U64 drawnSize = widen ? 4 : size;
    if (!primitive || widen)
    {
        // The indices as GL has them at the draw (metalIndexBytes: a static buffer's CPU copy, kept
        // with or without the debug setting, S8), in the list's order; the ring is 256-aligned.
        const std::span<const std::byte> bytes = metalIndexBytes(indices);
        if (bytes.size() < skip + U64(indexCount) * size)
        {
            LL_WARNS("RHI") << "drawIndexed: index buffer '" << indices->debugName()
                            << "' has no CPU copy of its indices for a fan, strip or loop" << LL_ENDL;
            return;
        }
        const std::vector<std::byte>& converted =
            indexConversion(indices, indexType, type, firstIndex, indexCount, widen, bytes);
        count = listCount;
        length = U64(count) * drawnSize;
        const MetalAllocation list = mDevice.frameAllocation(length);
        if (!list.buffer)
        {
            return; // the ring wrote the WARN line
        }
        // With the Metal debug setting the list's ring memory is resident too.
        if (mDevice.debugChecks() && !mDevice.residency().contains(list.buffer))
        {
            LL_WARNS("RHI") << "drawIndexed: the ring's list of index buffer '" << indices->debugName()
                            << "' is not resident" << LL_ENDL;
            return;
        }
        std::memcpy(list.cpu, converted.data(), size_t(length));
        address = list.buffer.gpuAddress + list.offset;
    }
    breakForCopies();
    if (!bindForDraw("drawIndexed", firstIndex, indexCount, true) || !open())
    {
        return;
    }
    if (!setNativePipeline())
    {
        return;
    }
    applyDrawState(type);
    [mNative drawIndexedPrimitives:drawn
                        indexCount:count
                         indexType:indexType == IndexType::U16 && !widen ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32
                       indexBuffer:address
                 indexBufferLength:length];
    ++mNativeDraws;
    mTracker.drawMade(); // the draw reached the native encoder
    mSegmentWork = true;
    if (mTracker.pipeline() && mTracker.pipeline()->native())
    {
        markNativeWrites(NativeStage::Vertex);
        markNativeWrites(NativeStage::Fragment);
    }
}

const std::vector<std::byte>& MetalEncoder::indexConversion(Buffer* indices, IndexType indexType, PrimitiveType type,
                                                            U32 first, U32 count, bool widen,
                                                            std::span<const std::byte> source)
{
    const U64 size = indexSize(indexType);
    const U64 drawnSize = widen ? 4 : size;
    const U64 skip = U64(first) * size;
    // The indices in the list's order, a native primitive's as they are, each widened to 32 bits with widen.
    const auto convert = [&](std::vector<std::byte>& out)
    {
        std::vector<U32> order;
        if (nativePrimitive(type))
        {
            order.resize(count);
            std::iota(order.begin(), order.end(), 0u);
        }
        else
        {
            order = metalListOrder(type, count);
        }
        out.resize(order.size() * drawnSize);
        for (size_t i = 0; i < order.size(); ++i)
        {
            if (widen)
            {
                U16 small = 0;
                std::memcpy(&small, source.data() + skip + order[i] * size, sizeof(small));
                const U32 wide = small;
                std::memcpy(out.data() + i * drawnSize, &wide, sizeof(wide));
            }
            else
            {
                std::memcpy(out.data() + i * size, source.data() + skip + order[i] * size, size);
            }
        }
    };
    const U64 id = indices->id();
    const U64 writes = metalBufferWrites(indices);
    ++mIndexConversionUses;
    for (IndexConversion& kept : mIndexConversions)
    {
        if (kept.buffer == id && kept.writes == writes && kept.first == first && kept.count == count &&
            kept.type == type && kept.indexType == indexType && kept.widen == widen)
        {
            kept.used = mIndexConversionUses;
            ++mIndexConversionHits;
            if (mDevice.debugChecks())
            {
                convert(mIndexScratch);
                if (mIndexScratch != kept.bytes)
                {
                    ++mResolveMismatches;
                    LL_WARNS("RHI") << "drawIndexed: the kept list of index buffer '" << indices->debugName()
                                    << "' differs from one made now" << LL_ENDL;
                    return mIndexScratch;
                }
            }
            return kept.bytes;
        }
    }
    ++mIndexConversionsMade;
    const U64 listed = nativePrimitive(type) ? count : metalListCount(type, count);
    if (listed * drawnSize > MAX_INDEX_CONVERSION_BYTES)
    {
        convert(mIndexScratch);
        return mIndexScratch;
    }
    if (mIndexConversions.size() >= MAX_INDEX_CONVERSIONS)
    {
        const auto oldest = std::min_element(mIndexConversions.begin(), mIndexConversions.end(),
                                             [](const IndexConversion& a, const IndexConversion& b)
                                             { return a.used < b.used; });
        mIndexConversions.erase(oldest);
    }
    IndexConversion& made = mIndexConversions.emplace_back();
    made.buffer = id;
    made.writes = writes;
    made.first = first;
    made.count = count;
    made.type = type;
    made.indexType = indexType;
    made.widen = widen;
    made.used = mIndexConversionUses;
    convert(made.bytes);
    return made.bytes;
}

bool MetalEncoder::setNativePipeline()
{
    // The variant without alpha for the segment's attachments that have none in GL, looked up again only when the
    // pipeline or the segment's mask is another than at the last draw's lookup. A pipeline's state never changes once
    // made, and its id is never reused.
    Pipeline* pipeline = mTracker.pipeline();
    if (!pipeline || !mDrawState || pipeline->id() != mDrawPipelineId || mNativeAlphaLess != mDrawAlphaLess)
    {
        mDrawState = metalPipelineState(pipeline, mDevice, mNativeAlphaLess);
        mDrawPipelineId = pipeline && mDrawState ? pipeline->id() : 0;
        mDrawAlphaLess = mNativeAlphaLess;
        if (!mDrawState)
        {
            return false; // its handler wrote the WARN line
        }
    }
    else if (mDevice.debugChecks() && mDrawState != metalPipelineState(pipeline, mDevice, mNativeAlphaLess))
    {
        // With the Metal debug setting the kept state is compared with a lookup.
        ++mResolveMismatches;
        LL_WARNS("RHI") << "the kept pipeline state of '" << pipeline->debugName() << "' differs from a lookup"
                        << LL_ENDL;
        mDrawState = metalPipelineState(pipeline, mDevice, mNativeAlphaLess);
    }
    if (mVertexWaitOpen)
    {
        // A vertex stage that samples (the translation's reflection) or reads native bindings (a native vertex
        // function's textures or buffers, which can change between draws of one pipeline) may read what an
        // earlier pass wrote, so the split barrier's geometry part comes before its first such draw.
        const MetalProgramLayout* layout = metalPipelineLayout(pipeline);
        const bool reads = pipeline && pipeline->native()
                               ? !mTracker.nativeTextures(NativeStage::Vertex).empty()
                                     || !mTracker.nativeBuffers(NativeStage::Vertex).empty()
                               : layout && !layout->vertex.textureUniforms.empty();
        if (reads)
        {
            mDevice.vertexBarrier(mNative);
            mVertexWaitOpen = false;
            if (mDevice.vertexBarriers() == 1)
            {
                LL_WARNS("RHI") << "a vertex stage samples a texture: its passes wait for the earlier work before "
                                << "their vertex work (counted in the speed counters)" << LL_ENDL;
            }
        }
    }
    if (mNativeStamp.wantsProgram)
    {
        mDevice.passTimes().noteProgram(mNativeStamp, mDrawState.label);
    }
    if (mDrawState != mNativePipeline)
    {
        [mNative setRenderPipelineState:mDrawState];
        mNativePipeline = mDrawState;
    }
    return true;
}

void MetalEncoder::waitForPipeline()
{
    // A Ready pipeline needs no wait; a Pending one is waited for, and a Failed one asked as before (a native
    // pipeline's failure writes its WARN line there).
    Pipeline* pipeline = mTracker.pipeline();
    if (pipeline && pipeline->status() != CompileStatus::Ready)
    {
        metalPipelineState(pipeline);
    }
}

void MetalEncoder::setIndexBuffer(Buffer* buffer, IndexType type)
{
    onRecordingThread("setIndexBuffer");
    // Recorded only: drawIndexed binds the memory current at the draw with the type then, so a type
    // change on the same buffer needs nothing here.
    accept("setIndexBuffer", mTracker.setIndexBuffer(buffer, type));
}
void MetalEncoder::setUniformBuffer(U32 blockIndex, Buffer* buffer, U64 offset, U64 size)
{
    onRecordingThread("setUniformBuffer");
    // The tracker keeps the buffer, the encoder its range, as GL does; each draw of a program
    // that reads the slot's block binds it on buffer 1 + slot (bindForDraw).
    accept("setUniformBuffer", mTracker.setUniformBuffer(blockIndex, buffer, offset, size));
    mBlockRanges[blockIndex] = { offset, size };
    if (blockIndex < mBlocks.size())
    {
        const auto bound = mTracker.uniformBuffers().find(blockIndex);
        mBlocks[blockIndex] = { bound != mTracker.uniformBuffers().end() ? bound->second.get() : nullptr, offset, size,
                                true };
    }
}

void MetalEncoder::updateBuffer(Buffer* buffer, U64 offset, std::span<const std::byte> data)
{
    onRecordingThread("updateBuffer");
    if (!accept("updateBuffer", mTracker.updateBuffer(buffer, offset, data.size())))
    {
        return;
    }
    if (buffer && !buffer->desc().dynamic && !data.empty())
    {
        // A static buffer's GPU copy runs between segments on the copy encoder, so a
        // draw before it reads the old bytes and the next segment loads what this one stored.
        splitPass();
    }
    metalUpdateBuffer(mDevice, buffer, offset, data);
}
void MetalEncoder::uploadTexture(Texture* texture, U32 level, U32 layer, const Region& region,
                                 const PixelLayout& layout, std::span<const std::byte> data)
{
    onRecordingThread("uploadTexture");
    if (!accept("uploadTexture", mTracker.uploadTexture(texture, level, layer, region, layout, data.size())))
    {
        return;
    }
    if (texture && texture->desc().samples > 1)
    {
        // A buffer-to-texture copy into a multisampled texture is a validation error (a run-time probe:
        // "sourceBytesPerRow(256) must be >= (1024)" for a row of 64 RGBA8 texels at 4x);
        // refused before any Metal call, as copyTexture and readback refuse theirs (the upload context's uploadTexture
        // comes here too).
        metalNotPorted("uploadTexture into a multisampled texture", "P3-08");
        return;
    }
    settleDepthClears(texture);
    // Metal 4 has no copy on a render encoder, so an upload in a pass breaks it; the
    // copy runs between segments, and the next draw opens a segment that loads what this one stored.
    if (mTracker.inPass())
    {
        ++mStats.uploadBreaks; // also when the split finds no segment to close
    }
    if (splitPass())
    {
        ++mStats.uploadPassBreaks; // this upload closed the open segment
    }
    metalUploadTexture(mDevice, texture, level, layer, region, layout, data);
}

// LLImageGL's in-place media update calls this before its write, so the write zeroes what
// lies outside its region, as the new texture of each update did (metalZeroAroundFirstWrite). A CPU mark
// only: the zeros go on the write's own encoder of copies, after the queue barrier, like the write.
void MetalEncoder::forgetWritten(Texture* texture, U32 level, U32 layer)
{
    onRecordingThread("forgetWritten");
    metalForgetWritten(texture, level, layer);
}

namespace
{
    /// How generateMipmaps makes the levels. "same" draws each level with the backend's own filter into a scratch it
    /// samples the level above from; "pingpong" draws into a second scratch instead; "blit" uses Metal 4's
    /// generateMipmapsForTexture on the texture itself (Metal's own filter). On an M2 Pro (Apple8) both drawn paths
    /// gave black, then transparent levels (conifer needles, mesh hair, clouds) and blit gave right ones; the
    /// M4s (Apple9) draw them right. So GPUs without MTLGPUFamilyApple9 (M1, M2) use blit, the others the drawn
    /// path, until the drawn path's fault on Apple8 is found. LL_METAL_MIPS=same|pingpong|blit overrides it (a
    /// diagnostic knob, not a setting). Chosen once, at the first mip generation, with a line in the log.
    enum class MipPath
    {
        PingPong,
        Same,
        Blit,
    };

    API_AVAILABLE(macos(26.0)) MipPath metalMipPath(IdMTLDevice device)
    {
        static const MipPath path = [device]()
        {
            const bool apple9 = [device supportsFamily:MTLGPUFamilyApple9];
            MipPath chosen = apple9 ? MipPath::Same : MipPath::Blit;
            const char* knob = getenv("LL_METAL_MIPS");
            if (knob)
            {
                chosen = std::strcmp(knob, "same") == 0       ? MipPath::Same
                         : std::strcmp(knob, "pingpong") == 0 ? MipPath::PingPong
                         : std::strcmp(knob, "blit") == 0     ? MipPath::Blit
                                                              : chosen;
            }
            LL_INFOS("RHI") << "Metal mipmaps: "
                            << (chosen == MipPath::Same ? "drawn" : chosen == MipPath::Blit ? "Metal's generateMipmaps"
                                                                                            : "drawn, two scratches")
                            << (knob ? std::string(" (LL_METAL_MIPS=") + knob + ")"
                                     : std::string(apple9 ? " (Apple9 GPU)" : " (no Apple9 GPU: M1 or M2)"))
                            << LL_ENDL;
            return chosen;
        }();
        return path;
    }

    // The copies Metal does not port, refused with metalNotPorted's WARN line and no Metal call; MetalEncoder::copyTexture
    // asks before its tracker, so a refused copy is never counted as a transfer. So is a copy between
    // formats whose draw cannot be made
    API_AVAILABLE(macos(26.0)) bool copyRefused(MetalDevice& device, const TextureDesc& from, const Region& srcRegion,
                                                const TextureDesc& to, U32 dstLevel, U32 dstX, U32 dstY)
    {
        if (from.samples > 1 || to.samples > 1)
        {
            // Blits and Metal 4's compute copies need equal sample counts (MTLBlitCommandEncoder.h:110,
            // 126; MTL4ComputeCommandEncoder.h:171, 186). The one copy ported is the resolve GL's blit makes of a whole
            // colour texture into a level of its format and size with one sample (resolveCopy). The rest is refused
            // before any Metal call: a level of another size is a validation error ("texture and resolveTexture size
            // mismatch", a run-time probe). validate() gives a multisampled texture one level and one layer
            // (llrhi.cpp:684-688).
            const bool resolve = from.samples > 1 && to.samples == 1 && from.format == to.format
                                 && !isDepth(from.format) && srcRegion.x == 0 && srcRegion.y == 0
                                 && srcRegion.width == from.width && srcRegion.height == from.height && dstX == 0
                                 && dstY == 0 && std::max(1u, to.width >> dstLevel) == from.width
                                 && std::max(1u, to.height >> dstLevel) == from.height;
            if (!resolve)
            {
                metalNotPorted("copyTexture of a multisampled texture other than a whole colour resolve of the "
                               "same size", "P3-08");
                return true;
            }
        }
        else if (const std::string why = copyRefusal(device.capabilities(), from.format, to.format); !why.empty())
        {
            // The one format rule LLGPUCopy::canFromFramebuffer asks too; sRGB, BC,
            // depth and integer formats only within their own
            metalNotPorted(("copyTexture between formats (" + why + ")").c_str(), "P3-08b, C4");
            return true;
        }
        else if (from.format != to.format)
        {
            // The draw's program and pipeline, as Device::copyRefusalBetween asks them for canFromFramebuffer.
            // A failure is not a port missing, so its line is not metalNotPorted's
            if (const std::string missing = device.formatCopyUnavailable(to.format); !missing.empty())
            {
                LL_WARNS("RHI") << "copyTexture between formats: " << missing << "; nothing is copied" << LL_ENDL;
                return true;
            }
        }
        return false;
    }
}

void MetalEncoder::copyTexture(Texture* src, U32 srcLevel, U32 srcLayer, const Region& srcRegion, Texture* dst,
                               U32 dstLevel, U32 dstLayer, U32 dstX, U32 dstY)
{
    onRecordingThread("copyTexture");
    // A copy the tracker would take but Metal does not port yet is refused before the tracker, so the
    // tracker does not count it (LLGPUCopy reports it as not delivered, and its caller's WARN_ONCE follows this one
    // line). The tracker's own refusals (no texture, a region outside a level) come first.
    if (src && dst && validateRegion(src->desc(), srcLevel, srcLayer, srcRegion).empty()
        && validateRegion(dst->desc(), dstLevel, dstLayer, { dstX, dstY, srcRegion.width, srcRegion.height }).empty()
        && copyRefused(mDevice, src->desc(), srcRegion, dst->desc(), dstLevel, dstX, dstY))
    {
        return;
    }
    if (!accept("copyTexture", mTracker.copyTexture(src, srcLevel, srcLayer, srcRegion, dst, dstLevel, dstLayer, dstX, dstY)))
    {
        return;
    }
    if (!src || !dst)
    {
        return;
    }
    settleDepthClears(src);
    settleDepthClears(dst);
    const TextureDesc& from = src->desc();
    // Metal 4 has no copy on a render encoder, so a copy in a pass breaks it, in GL's order: the
    // segment's draws are stored and copied, and the next draw opens a segment that loads them and
    // samples the copy. The resolve pass loads what the segment stored and stores it again (resolveCopy), so the
    // source keeps its samples, as GL's blit leaves it.
    splitPass();
    // A copy the tracker accepted that Metal then cannot make (no encoder, scratch or binding) is
    // taken back from the tracker, so LLGPUCopy reports it as not delivered (the WARN line is the failure's)
    if (from.samples > 1)
    {
        if (!resolveCopy(src, dst, dstLevel, dstLayer))
        {
            mTracker.transferDropped();
            return;
        }
    }
    else if (from.format != dst->desc().format)
    {
        // Between formats as a draw; within a format a blit (the hero copies among them)
        if (!formatCopy(src, srcLevel, srcLayer, srcRegion, dst, dstLevel, dstLayer, dstX, dstY))
        {
            mTracker.transferDropped();
            return;
        }
    }
    else
    {
        // On an encoder of its own after the queue barrier, so it copies what earlier work wrote, and
        // copies after it see what it wrote (MTL4ComputeCommandEncoder.h:242-250, MTL4CommandEncoder.h:55-58).
        IdMTL4ComputeCommandEncoder copies = mDevice.beginUploadCopies();
        if (!copies)
        {
            mTracker.transferDropped();
            return;
        }
        [copies copyFromTexture:metalTexture(src)
                    sourceSlice:srcLayer
                    sourceLevel:srcLevel
                   sourceOrigin:MTLOriginMake(srcRegion.x, srcRegion.y, 0)
                     sourceSize:MTLSizeMake(srcRegion.width, srcRegion.height, 1)
                      toTexture:metalTexture(dst)
               destinationSlice:dstLayer
               destinationLevel:dstLevel
              destinationOrigin:MTLOriginMake(dstX, dstY, 0)];
        // GL's zeros around a copy into part of a level and slice nothing has written
        metalZeroAroundFirstWrite(mDevice, copies, dst, dstLevel, dstLayer, { dstX, dstY, srcRegion.width, srcRegion.height });
        mDevice.endCopies();
    }
    metalMarkWritten(dst, dstLevel, dstLayer); // the copy's destination is written
    if (!hasAlphaInGLFormat(dst->desc().glFormat) && (dst->desc().usage & TEXTURE_RENDER_TARGET))
    {
        // GL drops the source's alpha in a target without alpha, and Metal's copy brings it.
        alphaAfterCopy(dst, dstLevel, dstLayer, { dstX, dstY, srcRegion.width, srcRegion.height });
    }
}

void MetalEncoder::alphaAfterCopy(Texture* dst, U32 level, U32 layer, const Region& region)
{
    PassFormats formats;
    formats.colorCount = 1;
    formats.colorFormats[0] = dst->desc().format;
    Program* program = mDevice.clearProgram(formats);
    if (!program)
    {
        return; // createProgram wrote the WARN line
    }
    PipelineDesc desc;
    applyPassFormats(formats, desc);
    desc.attributeMask = 0;
    desc.colorMask = COLOR_MASK_A; // alpha only, and blending off
    Ref<Pipeline> pipeline = mDevice.clearPipeline(*program, desc);
    IdMTLRenderPipelineState state = metalPipelineState(pipeline.get()); // waited for; nil after a WARN line
    if (!state)
    {
        return;
    }
    // The level and layer of the destination on an encoder of its own after the queue barrier, so it draws over
    // what the copy wrote (MTL4CommandEncoder.h:55-58); the scissor is the copied region.
    MTL4RenderPassDescriptor* pass = [MTL4RenderPassDescriptor new];
    pass.colorAttachments[0].texture = metalTexture(dst);
    pass.colorAttachments[0].level = level;
    pass.colorAttachments[0].slice = layer;
    pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    IdMTL4RenderCommandEncoder encoder = openOwnEncoder(pass, @"Alpha after copy");
    if (!encoder)
    {
        LL_WARNS("RHI") << "copyTexture: Metal gave no render encoder for the alpha of '" << dst->debugName() << "'"
                        << LL_ENDL;
        return;
    }
    const U32 width = std::max(1u, dst->desc().width >> level);
    const U32 height = std::max(1u, dst->desc().height >> level);
    const U32 x0 = std::min(region.x, width);
    const U32 y0 = std::min(region.y, height);
    [encoder setScissorRect:{ x0, y0, std::min(region.width, width - x0), std::min(region.height, height - y0) }];
    // bindForClear writes the tables, which the encoder takes at its draw (MTL4RenderCommandEncoder.h:613-622).
    if (bindForClear(*program, { 0.f, 0.f, 0.f, 1.f }, 1.f))
    {
        [encoder setArgumentTable:mTables[0] atStages:MTLRenderStageVertex];
        [encoder setArgumentTable:mTables[1] atStages:MTLRenderStageFragment];
        [encoder setRenderPipelineState:state];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        ++mAlphaCopyDraws;
    }
    closeOwnEncoder(encoder);
}

bool MetalEncoder::formatCopy(Texture* src, U32 srcLevel, U32 srcLayer, const Region& srcRegion, Texture* dst,
                              U32 dstLevel, U32 dstLayer, U32 dstX, U32 dstY)
{
    // copyRefused has had both made, so these are the cache's
    Program* program = mDevice.formatCopyProgram();
    if (!program)
    {
        return false; // createProgram wrote the WARN line; there is no MSL of the backend's own
    }
    // The copy waits for a pipeline state being made; nil after a WARN line.
    Ref<Pipeline> pipeline = mDevice.formatCopyPipeline(*program, MetalDevice::formatCopyPipelineDesc(dst->desc().format));
    IdMTLRenderPipelineState state = metalPipelineState(pipeline.get());
    if (!state)
    {
        return false;
    }
    // Two 2D scratches of the region with no swizzle, as generateMipmaps': the source's level and
    // layer are blitted into the first, so the draw reads their stored components and not a swizzled R8's or RG8's
    // view, and the second is the attachment in the destination's format, which a swizzled R8 or RG8 destination
    // cannot be. Resident from creation, released through FrameLifecycle::release.
    const auto scratchOf = [&srcRegion](const TextureDesc& of)
    {
        TextureDesc desc;
        desc.format = of.format;
        desc.glFormat = of.glFormat;
        desc.width = srcRegion.width;
        desc.height = srcRegion.height;
        desc.usage = TEXTURE_SAMPLED | TEXTURE_RENDER_TARGET;
        return desc;
    };
    Ref<Texture> from = mDevice.createTexture(scratchOf(src->desc()), "format copy source");
    Ref<Texture> to = mDevice.createTexture(scratchOf(dst->desc()), "format copy");
    if (from.isNull() || to.isNull())
    {
        return false; // createTexture wrote the WARN line
    }
    // Each copy and the draw on an encoder of its own after the queue barrier, so each reads what the one before it
    // wrote (MTL4CommandEncoder.h:55-58, 70-72).
    IdMTL4ComputeCommandEncoder copies = mDevice.beginUploadCopies();
    if (!copies)
    {
        return false;
    }
    [copies copyFromTexture:metalTexture(src)
                sourceSlice:srcLayer
                sourceLevel:srcLevel
               sourceOrigin:MTLOriginMake(srcRegion.x, srcRegion.y, 0)
                 sourceSize:MTLSizeMake(srcRegion.width, srcRegion.height, 1)
                  toTexture:metalTexture(from.get())
           destinationSlice:0
           destinationLevel:0
          destinationOrigin:MTLOriginMake(0, 0, 0)];
    mDevice.endCopies();
    // Every texel of the attachment is written, so nothing is loaded (MTLRenderPass.h:33-35).
    MTL4RenderPassDescriptor* pass = [MTL4RenderPassDescriptor new];
    pass.colorAttachments[0].texture = metalTexture(to.get());
    pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    IdMTL4RenderCommandEncoder encoder = openOwnEncoder(pass, @"Format copy");
    if (!encoder)
    {
        LL_WARNS("RHI") << "copyTexture: Metal gave no render encoder for the copy into '" << dst->debugName() << "'"
                        << LL_ENDL;
        return false;
    }
    // The encoder's defaults: the viewport and the scissor are the attachment, no culling and no depth.
    const bool bound = bindForFormatCopy(*program, from.get(), encoder);
    if (bound)
    {
        [encoder setRenderPipelineState:state];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        ++mFormatCopyDraws;
    }
    closeOwnEncoder(encoder);
    if (!bound)
    {
        return false;
    }
    copies = mDevice.beginUploadCopies();
    if (!copies)
    {
        return false;
    }
    [copies copyFromTexture:metalTexture(to.get())
                sourceSlice:0
                sourceLevel:0
               sourceOrigin:MTLOriginMake(0, 0, 0)
                 sourceSize:MTLSizeMake(srcRegion.width, srcRegion.height, 1)
                  toTexture:metalTexture(dst)
           destinationSlice:dstLayer
           destinationLevel:dstLevel
          destinationOrigin:MTLOriginMake(dstX, dstY, 0)];
    // GL's zeros around a copy into part of a level and slice nothing has written
    metalZeroAroundFirstWrite(mDevice, copies, dst, dstLevel, dstLayer, { dstX, dstY, srcRegion.width, srcRegion.height });
    mDevice.endCopies();
    return true;
}

bool MetalEncoder::resolveCopy(Texture* src, Texture* dst, U32 level, U32 layer)
{
    // The source loads and its store action resolves it into the level and slice of the resolve texture
    // (MTLRenderPass.h:42, 97-113; MTL4RenderPass.h:33); the pass draws nothing. A run-time probe: such an empty Metal 4
    // pass resolves into a destination with usage RenderTarget, Unknown or ShaderRead with no validation line. Store
    // and resolve, not MultisampleResolve alone (MTLRenderPass.h:41-42 are separate actions): the samples are stored
    // too, so a later Load of the source (the next segment of a pass, or another resolve) reads them, as GL's
    // glBlitFramebuffer leaves its source (llrhigl.cpp copyTexture).
    MTL4RenderPassDescriptor* pass = [MTL4RenderPassDescriptor new];
    pass.colorAttachments[0].texture = metalTexture(src);
    pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
    pass.colorAttachments[0].storeAction = MTLStoreActionStoreAndMultisampleResolve;
    pass.colorAttachments[0].resolveTexture = metalTexture(dst);
    pass.colorAttachments[0].resolveLevel = level;
    pass.colorAttachments[0].resolveSlice = layer;
    // The queue barrier: the resolve loads the source after the drawing pass's store has landed
    // (MTL4CommandEncoder.h:50-52).
    IdMTL4RenderCommandEncoder encoder = openOwnEncoder(pass, @"Resolve");
    if (!encoder)
    {
        LL_WARNS("RHI") << "copyTexture: Metal gave no render encoder for the resolve of '" << src->debugName()
                        << "'" << LL_ENDL;
        return false;
    }
    closeOwnEncoder(encoder);
    return true;
}

bool MetalEncoder::upscale(Upscaler* upscaler, Texture* input, U32 width, U32 height, Texture* output)
{
    onRecordingThread("upscale");
    if (!accept("upscale", mTracker.upscale(upscaler, input, width, height, output)))
    {
        return false;
    }
    // What MetalFX is given must be what the scaler was made for, also with the tracker's checks off: other sizes or
    // formats are not MetalFX's to refuse (MTLFXSpatialScaler.h, "Assigning input and output textures"), and its
    // output must be Private (the backend's textures are, unless memoryless).
    IdMTL4FXSpatialScaler scaler = metalScaler(upscaler);
    IdMTLFence fence = metalScalerFence(upscaler);
    IdMTLTexture from = metalTexture(input);
    IdMTLTexture to = metalTexture(output);
    if (!scaler || !fence || !from || !to || from.width != scaler.inputWidth || from.height != scaler.inputHeight
        || from.pixelFormat != scaler.colorTextureFormat || to.width != scaler.outputWidth
        || to.height != scaler.outputHeight || to.pixelFormat != scaler.outputTextureFormat
        || to.storageMode != MTLStorageModePrivate || width == 0 || height == 0 || width > from.width
        || height > from.height)
    {
        LL_WARNS("RHI") << "upscale: the textures are not the ones the upscaler was made for" << LL_ENDL;
        return false;
    }
    // MetalFX records encoders of its own on the command buffer, so the open segment ends first, as for a copy (its
    // draws are stored, and a stack pass goes on in a new segment that loads them).
    splitPass();
    // Resources are untracked on Metal 4, so the scaler's work is ordered by hand. An encoder of its own begins with
    // the queue barrier (openCopies): it runs after the queue's earlier work, which wrote the input and read the last
    // output. Its producer barrier keeps every later encoder's stages, MetalFX's included, from starting before that
    // work is done (MTL4CommandEncoder.h:74-93), and it updates the scaler's fence after it
    // (MTL4CommandEncoder.h:117-131), the fence MetalFX takes to synchronize untracked resources
    // (MTLFXSpatialScaler.h); MetalFX's Metal 4 scaler stops at an assertion without one (a run-time probe). After
    // the scaler, the next encoder's own queue barrier waits for all of its work (MetalDevice::queueBarrier and
    // passBarrier wait after every stage of the queue), so what reads the output reads what it wrote.
    IdMTL4ComputeCommandEncoder before = mDevice.beginUploadCopies(@"Upscale");
    if (!before)
    {
        return false; // openCopies wrote the WARN line
    }
    [before barrierAfterStages:MTLStageAll beforeQueueStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice];
    [before updateFence:fence afterEncoderStages:MTLStageDispatch | MTLStageBlit];
    mDevice.endCopies();
    scaler.colorTexture = from;
    scaler.inputContentWidth = width;
    scaler.inputContentHeight = height;
    scaler.outputTexture = to;
    scaler.fence = fence;
    [scaler encodeToCommandBuffer:mDevice.commandBuffer()];
    // The scaler keeps no texture past the work it recorded, so a texture let go is freed with its frames.
    scaler.colorTexture = nil;
    scaler.outputTexture = nil;
    metalMarkWritten(output, 0, 0);
    ++mUpscales;
    return true;
}

bool MetalEncoder::upscaleTemporal(TemporalUpscaler* upscaler, const TemporalUpscaleInputs& inputs)
{
    onRecordingThread("upscaleTemporal");
    if (!accept("upscaleTemporal", mTracker.upscaleTemporal(upscaler, inputs)))
    {
        return false;
    }
    // What MetalFX is given must be what the scaler was made for, also with the tracker's checks off
    // (MTLFXTemporalScaler.h, "Assigning input and output textures"), and its output must be Private.
    IdMTL4FXTemporalScaler scaler = metalTemporalScaler(upscaler);
    IdMTLFence fence = metalTemporalScalerFence(upscaler);
    IdMTLTexture exposure = metalTemporalScalerExposure(upscaler);
    IdMTLTexture color = metalTexture(inputs.color);
    IdMTLTexture depth = metalTexture(inputs.depth);
    IdMTLTexture motion = metalTexture(inputs.motion);
    IdMTLTexture reactive = inputs.reactive ? metalTexture(inputs.reactive) : nil;
    IdMTLTexture to = metalTexture(inputs.output);
    if (!scaler || !fence || !exposure || !color || !depth || !motion || !to || (inputs.reactive && !reactive)
        || color.width != scaler.inputWidth || color.height != scaler.inputHeight
        || color.pixelFormat != scaler.colorTextureFormat || depth.pixelFormat != scaler.depthTextureFormat
        || motion.pixelFormat != scaler.motionTextureFormat || to.width != scaler.outputWidth
        || to.height != scaler.outputHeight || to.pixelFormat != scaler.outputTextureFormat
        || to.storageMode != MTLStorageModePrivate || inputs.width == 0 || inputs.height == 0
        || inputs.width > color.width || inputs.height > color.height)
    {
        LL_WARNS("RHI") << "upscaleTemporal: the textures are not the ones the upscaler was made for" << LL_ENDL;
        return false;
    }
    // Recorded as upscale's spatial scaler is: the open segment ends, an encoder of its own waits for the queue's
    // earlier work (which wrote every input and read the last output) and updates the scaler's fence, and the next
    // encoder's queue barrier orders what reads the output after it.
    splitPass();
    IdMTL4ComputeCommandEncoder before = mDevice.beginUploadCopies(@"Upscale temporal");
    if (!before)
    {
        return false; // openCopies wrote the WARN line
    }
    [before barrierAfterStages:MTLStageAll beforeQueueStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice];
    [before updateFence:fence afterEncoderStages:MTLStageDispatch | MTLStageBlit];
    mDevice.endCopies();
    scaler.colorTexture = color;
    scaler.depthTexture = depth;
    scaler.motionTexture = motion;
    scaler.reactiveMaskTexture = reactive;
    scaler.exposureTexture = exposure;
    scaler.outputTexture = to;
    scaler.inputContentWidth = inputs.width;
    scaler.inputContentHeight = inputs.height;
    scaler.jitterOffsetX = inputs.jitterX;
    scaler.jitterOffsetY = inputs.jitterY;
    scaler.motionVectorScaleX = inputs.motionScaleX;
    scaler.motionVectorScaleY = inputs.motionScaleY;
    scaler.preExposure = 1.f;
    scaler.depthReversed = NO;
    scaler.reset = inputs.reset ? YES : NO;
    scaler.fence = fence;
    [scaler encodeToCommandBuffer:mDevice.commandBuffer()];
    // The scaler keeps no texture past the work it recorded (its history is its own), so a texture let go is freed
    // with its frames.
    scaler.colorTexture = nil;
    scaler.depthTexture = nil;
    scaler.motionTexture = nil;
    scaler.reactiveMaskTexture = nil;
    scaler.outputTexture = nil;
    metalMarkWritten(inputs.output, 0, 0);
    ++mUpscales;
    return true;
}

void MetalEncoder::generateMipmaps(Texture* texture)
{
    onRecordingThread("generateMipmaps");
    if (!accept("generateMipmaps", mTracker.generateMipmaps(texture)))
    {
        return;
    }
    if (!texture || texture->desc().mipLevels == 1)
    {
        // A cube with one level describes no mips, so there is nothing to make.
        return;
    }
    settleDepthClears(texture);
    // In a pass the segment's draws go first and sample the old levels, as on GL; the next draw opens
    // a segment that loads its attachments.
    splitPass();
    // The backend's own filter, not Metal 4's generateMipmapsForTexture,
    // which takes each block's bottom-right texel (a probe); with no mip program there are no mips.
    const TextureDesc& desc = texture->desc();
    PassFormats formats;
    formats.colorCount = 1;
    formats.colorFormats[0] = desc.format;
    PipelineDesc pipelineDesc;
    applyPassFormats(formats, pipelineDesc);
    pipelineDesc.attributeMask = 0;
    Program* program = mDevice.mipProgram();
    if (!program)
    {
        return; // createProgram wrote the WARN line; there is no MSL of the backend's own
    }
    // The mips wait for a pipeline state being made; nil after a WARN line.
    Ref<Pipeline> pipeline = mDevice.mipPipeline(*program, pipelineDesc);
    IdMTLRenderPipelineState state = metalPipelineState(pipeline.get());
    if (!state)
    {
        return;
    }
    // LLImageGL's textures have no render target usage (llimagegl.cpp:2926), and a swizzled R8 or RG8 texture
    // cannot have it, so the levels are drawn in a 2D scratch of the texture's format, size and levels with no
    // swizzle; the copies move the bytes. Resident from creation, released through FrameLifecycle::release.
    IdMTLTexture target = metalTexture(texture);
    // A cube's slice is its face, a cube array's cube * 6 + face (createMetalTexture).
    const U32 slices = desc.type == TextureType::Tex2D ? 1 : 6 * (desc.type == TextureType::CubeArray ? desc.layers : 1);
    const MipPath path = metalMipPath(mDevice.mtlDevice());
    if (path == MipPath::Blit)
    {
        // Metal's own filter on the texture itself, all slices and levels (the path on M1 and M2: metalMipPath).
        IdMTL4ComputeCommandEncoder copies = mDevice.beginUploadCopies();
        if (!copies)
        {
            return;
        }
        [copies generateMipmapsForTexture:target];
        mDevice.endCopies();
        for (U32 slice = 0; slice < slices; ++slice)
        {
            for (U32 level = 1; level < desc.mipLevels; ++level)
            {
                metalMarkWritten(texture, level, slice);
            }
        }
        return;
    }
    TextureDesc scratchDesc = desc;
    scratchDesc.type = TextureType::Tex2D;
    scratchDesc.layers = 1;
    scratchDesc.swizzle = SwizzleRGBA();
    scratchDesc.usage = TEXTURE_SAMPLED | TEXTURE_RENDER_TARGET;
    // With LL_METAL_MIPS=pingpong, two scratches, a level drawn into the one it does not sample: level 0 and every
    // even level in the first, the odd levels in the second (a test on the M2 Pro: it did not help there).
    Ref<Texture> scratch = mDevice.createTexture(scratchDesc, "mip scratch");
    if (scratch.isNull())
    {
        return; // createTexture wrote the WARN line
    }
    Ref<Texture> scratchOdd = scratch;
    if (path == MipPath::PingPong && desc.mipLevels > 1)
    {
        scratchOdd = mDevice.createTexture(scratchDesc, "mip scratch odd");
        if (scratchOdd.isNull())
        {
            return; // createTexture wrote the WARN line
        }
    }
    const auto holder = [&](U32 level) -> Texture* { return (level % 2) ? scratchOdd.get() : scratch.get(); };
    for (U32 slice = 0; slice < slices; ++slice)
    {
        // Each copy and each level on an encoder of its own after the queue barrier, so it reads what the
        // encoder before it wrote (MTL4CommandEncoder.h:55-58, 70-72).
        IdMTL4ComputeCommandEncoder copies = mDevice.beginUploadCopies();
        if (!copies)
        {
            return;
        }
        [copies copyFromTexture:target sourceSlice:slice sourceLevel:0 toTexture:metalTexture(scratch.get())
               destinationSlice:0 destinationLevel:0 sliceCount:1 levelCount:1];
        mDevice.endCopies();
        for (U32 level = 1; level < desc.mipLevels; ++level)
        {
            // The level above as a view of its own (MTLTexture.h:535), in the scratch that holds it; the level is
            // drawn in the other scratch (or, with LL_METAL_MIPS=same, in the same one: MTLRenderPass.h:82).
            Texture* source = holder(level - 1);
            IdMTLTexture sourceLevels = metalTexture(source);
            IdMTLTexture levels = metalTexture(holder(level));
            IdMTLTexture above = [sourceLevels newTextureViewWithPixelFormat:sourceLevels.pixelFormat
                                                                 textureType:MTLTextureType2D
                                                                      levels:NSMakeRange(level - 1, 1)
                                                                      slices:NSMakeRange(0, 1)];
            MTL4RenderPassDescriptor* pass = [MTL4RenderPassDescriptor new];
            pass.colorAttachments[0].texture = levels;
            pass.colorAttachments[0].level = level;
            pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
            pass.colorAttachments[0].storeAction = MTLStoreActionStore;
            IdMTL4RenderCommandEncoder encoder = above ? openOwnEncoder(pass, @"Mips") : nil;
            if (!encoder)
            {
                LL_WARNS("RHI") << "generateMipmaps: Metal gave no " << (above ? "render encoder" : "texture view")
                                << " for level " << level << " of '" << texture->debugName() << "'" << LL_ENDL;
                return;
            }
            // The encoder's defaults: the viewport and the scissor are the level, no culling and no depth.
            const bool bound = bindForMip(*program, source, above, encoder);
            if (bound)
            {
                [encoder setRenderPipelineState:state];
                [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                ++mMipDraws;
            }
            closeOwnEncoder(encoder);
            // Metal 4 does not retain what a command buffer uses, so the view lives until the frame is done; then
            // the shadows forget it before it is freed and its MTLResourceID can come back (forgetTexture).
            mDevice.lifecycle().release([this, above]() { forgetTexture(above.gpuResourceID); });
            if (!bound)
            {
                return;
            }
        }
        copies = mDevice.beginUploadCopies();
        if (!copies)
        {
            return;
        }
        if (scratchOdd == scratch)
        {
            [copies copyFromTexture:metalTexture(scratch.get()) sourceSlice:0 sourceLevel:1 toTexture:target
                   destinationSlice:slice destinationLevel:1 sliceCount:1 levelCount:desc.mipLevels - 1];
        }
        else
        {
            for (U32 level = 1; level < desc.mipLevels; ++level)
            {
                [copies copyFromTexture:metalTexture(holder(level)) sourceSlice:0 sourceLevel:level toTexture:target
                       destinationSlice:slice destinationLevel:level sliceCount:1 levelCount:1];
            }
        }
        mDevice.endCopies();
        for (U32 level = 1; level < desc.mipLevels; ++level)
        {
            metalMarkWritten(texture, level, slice); // the generated levels are written
        }
    }
}

bool MetalEncoder::presentDraw(IdMTLTexture drawable, Texture* back)
{
    // The frame's work goes first. The open segment ends with color and depth stored and its readbacks'
    // copies encoded after it; the pass goes on in a new segment that loads, so a facade's stack pass stays open
    // as its record says, and a draw after the present (display_startup's depth clear) opens that segment.
    splitPass();
    mDevice.endCopies();
    PassFormats formats;
    formats.colorCount = 1;
    formats.colorFormats[0] = Format::BGRA8Unorm; // the layer's pixel format (MetalSurface)
    PipelineDesc pipelineDesc;
    applyPassFormats(formats, pipelineDesc);
    pipelineDesc.attributeMask = 0;
    Program* program = mDevice.presentProgram();
    if (!program)
    {
        return false; // createProgram wrote the WARN line; there is no MSL of the backend's own
    }
    // The flip pass waits for a pipeline state being made; nil after a WARN line.
    Ref<Pipeline> pipeline = mDevice.presentPipeline(*program, pipelineDesc);
    IdMTLRenderPipelineState state = metalPipelineState(pipeline.get());
    if (!state)
    {
        return false;
    }
    // The tables are written before the pass, so a binding that fails opens no pass on the drawable. With no encoder
    // yet, bindForPresent sets them on none (a message to nil), and they are set on the encoder once it is open.
    if (!bindForPresent(*program, back, nil))
    {
        return false;
    }
    // Every drawable pixel is written, so nothing is loaded (MTLRenderPass.h:33-35).
    MTL4RenderPassDescriptor* pass = [MTL4RenderPassDescriptor new];
    pass.colorAttachments[0].texture = drawable;
    pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    IdMTL4RenderCommandEncoder encoder = openOwnEncoder(pass, @"Present");
    if (!encoder)
    {
        LL_WARNS("RHI") << "present: Metal gave no render encoder for the drawable" << LL_ENDL;
        return false;
    }
    // The encoder's defaults: the viewport and the scissor are the drawable, no culling and no depth. Metal takes the
    // tables at the draw (MTL4RenderCommandEncoder.h:613-622).
    [encoder setArgumentTable:mTables[0] atStages:MTLRenderStageVertex];
    [encoder setArgumentTable:mTables[1] atStages:MTLRenderStageFragment];
    [encoder setRenderPipelineState:state];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    ++mPresentDraws;
    closeOwnEncoder(encoder);
    return true;
}

Ref<Readback> MetalEncoder::readback(Texture* texture, U32 level, U32 layer, const Region& region,
                                     const PixelLayout& layout)
{
    onRecordingThread("readback");
    if (!accept("readback", mTracker.readback(texture, level, layer, region)))
    {
        return nullptr;
    }
    // A multisampled texture is never read back here. A buffer copy from one is a validation error (a
    // run-time probe: "destinationBytesPerRow(64) must be >= (256)"), so createTexture refuses one with
    // TEXTURE_READBACK (textureRefusal), and the tracker, whose checks are always on on Metal, refuses a readback of a
    // texture without it.
    // The identity layout of the format only, and RGB of U8 from an RGBA8 texture (the window's
    // snapshots, rawSnapshot and saveSnapshot, read GL_RGB): its identity RGBA copy, packed to RGB by the readback.
    // Other conversions are not ported (metalNotPorted).
    const std::optional<PixelLayout> identity = texture ? metalReadbackLayout(texture->desc().format) : std::nullopt;
    std::optional<PixelLayout> rgb = identity;
    if (rgb)
    {
        rgb->components = PixelLayout::Components::RGB;
    }
    const bool packRGB = identity && identity->components == PixelLayout::Components::RGBA &&
                         identity->type == PixelLayout::Type::U8 && layout == *rgb;
    if (!identity || !(layout == *identity || packRGB))
    {
        metalNotPorted("readback of another format or layout", "P3-08");
        return nullptr;
    }
    // Tight rows, as GL's (GL_PACK_ALIGNMENT 1); a packed readback's rows are 3 bytes a pixel.
    const U32 rowPitch = region.width * identity->bytesPerPixel();
    IdMTLBuffer buffer = mDevice.newReadbackBuffer(U64(rowPitch) * region.height);
    if (!buffer)
    {
        return nullptr;
    }
    settleDepthClears(texture);
    const MetalCopy copy{ metalTexture(texture), level, layer, region, buffer, rowPitch,
                          hasStencil(texture->desc().format) ? MTLBlitOptionDepthFromDepthStencil : MTLBlitOptionNone,
                          std::make_shared<MetalReadbackStamp>() };
    // Four components of a texture without alpha in GL read back with alpha 1.
    const bool alphaOne =
        identity->components == PixelLayout::Components::RGBA && !hasAlphaInGLFormat(texture->desc().glFormat);
    Ref<Readback> result = new MetalReadback(mDevice, buffer, packRGB ? region.width * 3 : rowPitch, copy.stamp, alphaOne,
                                             identity->type, packRGB);
    if (mTracker.inPass())
    {
        // The copy waits for the segment to break, so it reads what was recorded before it.
        mPendingCopies.push_back(copy);
    }
    else
    {
        mDevice.encodeCopy(copy);
    }
    return result;
}

Ref<Readback> MetalEncoder::readback(Buffer* buffer, U64 offset, U64 size)
{
    onRecordingThread("readback");
    if (!accept("readback", mTracker.readback(buffer, offset, size)))
    {
        return nullptr;
    }
    // An ordered copy on the GPU with a stamp, as a texture's readback, not a memcpy at the call: a
    // static buffer's updates reach its memory through GPU copies (metalUpdateBuffer), so the read follows them and
    // gives GL's bytes at the call (glGetBufferSubData). A dynamic buffer is read from its current memory, which an
    // update replaces. Draws write no buffer, so a pass is split only because Metal 4 has no copy on a render
    // encoder, as updateBuffer splits it.
    const MetalAllocation source = metalAllocation(buffer);
    // The tracker refuses an empty range (validateRange), so size is at least 1.
    IdMTLBuffer copy = source.buffer ? mDevice.newReadbackBuffer(size) : nil;
    if (!copy)
    {
        return nullptr;
    }
    auto stamp = std::make_shared<MetalReadbackStamp>();
    Ref<Readback> result = new MetalReadback(mDevice, copy, U32(size), stamp);
    splitPass();
    mDevice.encodeBufferCopy(source.buffer, source.offset + offset, copy, size, stamp);
    return result;
}
// Queries --------------------------------------------------------------------------------

namespace
{
    bool sampleQuery(QueryType type)
    {
        return type == QueryType::AnySamplesPassed || type == QueryType::SamplesPassed;
    }
}

bool MetalEncoder::beginQuery(Query* query)
{
    onRecordingThread("beginQuery");
    const std::vector<Ref<Query>>& active = mTracker.activeQueries();
    const bool wasActive = std::any_of(active.begin(), active.end(),
                                       [query](const Ref<Query>& one) { return one.get() == query; });
    if (!accept("beginQuery", mTracker.beginQuery(query)))
    {
        if (query && !wasActive && sampleQuery(query->type()))
        {
            // A refused begin forgets the last result, as an accepted one does, so a pooled query never gives another
            // group's answer; with none it reads as visible (MetalQuery). The active query itself stays active.
            std::shared_ptr<MetalQueryRun> refused = std::make_shared<MetalQueryRun>();
            refused->ended = true;
            static_cast<MetalQuery*>(query)->mRun = refused;
        }
        else if (query && !wasActive && query->type() == QueryType::TimeElapsed && mDevice.timestampHeap())
        {
            // Likewise for a time query (another one active, the tracker's class 1): 0 ns, timer_invalid.
            std::shared_ptr<MetalTimerRun> refused = std::make_shared<MetalTimerRun>();
            refused->ended = true;
            static_cast<MetalTimerQuery*>(query)->mRun = refused; // createQueries makes only MetalTimerQuery for it
        }
        return false;
    }
    if (query->type() == QueryType::TimeElapsed && mDevice.timestampHeap())
    {
        beginTimer(*static_cast<MetalTimerQuery*>(query)); // createQueries makes only MetalTimerQuery for the kind
        return true;
    }
    if (!sampleQuery(query->type()))
    {
        // createQueries makes none (primitive queries are GL's only; time queries need the heap); another
        // backend's query of such a kind is refused.
        mTracker.endQuery(query);
        LL_WARNS("RHI") << "beginQuery: " << (query->type() == QueryType::TimeElapsed ? "time" : "primitive")
                        << " queries are not made on this Metal device; refused" << LL_ENDL;
        return false;
    }
    MetalQuery* metal = static_cast<MetalQuery*>(query); // createQueries makes only MetalQuery for the sample kinds
    std::shared_ptr<MetalQueryRun> run = std::make_shared<MetalQueryRun>();
    run->slot = mDevice.takeVisibilitySlot(run);
    run->buffer = mDevice.visibilityBuffer();
    metal->mRun = run;
    mActiveRun = run;
    // Boolean for "at least one sample", the count for SamplesPassed (MTLRenderCommandEncoder.h:38-42).
    mActiveMode = query->type() == QueryType::SamplesPassed ? MTLVisibilityResultModeCounting
                                                            : MTLVisibilityResultModeBoolean;
    if (mNative && run->slot >= 0 && !mOpenPass.visibilityResultBuffer)
    {
        // The open segment began before the buffer existed (the first createQueries): the next draw opens one with it.
        splitPass();
    }
    return true; // the draws set the mode (setNativeVisibility)
}

void MetalEncoder::endQuery(Query* query)
{
    onRecordingThread("endQuery");
    if (!accept("endQuery", mTracker.endQuery(query)))
    {
        return;
    }
    if (query->type() == QueryType::TimeElapsed)
    {
        endTimer(*static_cast<MetalTimerQuery*>(query)); // only a MetalTimerQuery's begin was accepted (beginQuery)
        return;
    }
    MetalQuery* metal = static_cast<MetalQuery*>(query);
    if (metal->mRun == mActiveRun)
    {
        mActiveRun.reset(); // the next draw turns the mode off (setNativeVisibility)
    }
    if (metal->mRun)
    {
        metal->mRun->ended = true;
        mDevice.stampNextSubmit(metal->mRun->stamp); // its result is ready once that submit has finished
    }
}

void MetalEncoder::abandonQuery()
{
    if (mActiveRun)
    {
        mActiveRun->abandoned = true; // no end will come: it reads as visible
        mActiveRun.reset();
    }
    if (mActiveTimer)
    {
        mActiveTimer->abandoned = true; // no end will come: 0 ns
        mActiveTimer.reset();
    }
}

void MetalEncoder::beginTimer(MetalTimerQuery& query)
{
    // A begin takes a new pair and forgets the last result (llrhi.h's Query contract), as a sample query's does.
    std::shared_ptr<MetalTimerRun> run = std::make_shared<MetalTimerRun>();
    query.mRun = run;
    mActiveTimer = run;
    run->pair = mDevice.takeTimerPair(run);
    if (run->pair >= 0 && writeTimestamp(U32(run->pair) * 2))
    {
        run->beginWritten = true;
        mDevice.stampNextSubmit(run->beginStamp); // the submit that carries the stamp, for its feedback
    }
}

void MetalEncoder::endTimer(MetalTimerQuery& query)
{
    const std::shared_ptr<MetalTimerRun> run = query.mRun;
    if (!run)
    {
        return;
    }
    if (run == mActiveTimer)
    {
        mActiveTimer.reset();
    }
    run->ended = true;
    if (run->beginWritten && !run->abandoned && writeTimestamp(U32(run->pair) * 2 + 1))
    {
        run->endWritten = true;
        mDevice.stampNextSubmit(run->endStamp); // its result is ready once that submit has finished
    }
}

bool MetalEncoder::writeTimestamp(U32 index)
{
    // The stamp lies on a boundary of the pass. An open native encoder (it has drawn) ends here, so its draws come
    // before the stamp, and the next draw opens one that loads what it stored. A command buffer's stamp is the
    // documented order (MTL4CommandBuffer.h:162-174); a render encoder's Relaxed stamp may sample at its boundaries
    // and a Precise one may split it anyway (MTL4Counters.h). With no native encoder open (right after a pass's
    // begin and its clear, which is a load action, or after a split) there is nothing to split, so a query begun before
    // the first draw and ended after the last costs no extra encoder.
    if (mCompute)
    {
        return false; // no stamp on the command buffer while a compute encoder is open: the query has no result
    }
    if (mNative && splitPass())
    {
        ++mStats.timerSplits;
    }
    return mDevice.writeTimestamp(index);
}

// Compute passes and native bindings ------------------------------------------------------------------------

bool MetalEncoder::beginComputePass(const ComputePassDesc& pass)
{
    onRecordingThread("beginComputePass");
    // An open stack pass is suspended: what it recorded is stored first, as for a copy.
    const bool suspends = mTracker.inPass() && mTracker.pass().stack && !mTracker.suspendedPass();
    if (suspends)
    {
        splitPass();
    }
    if (!accept("beginComputePass", mTracker.beginComputePass(pass)))
    {
        return false;
    }
    if (suspends)
    {
        mSegment = RenderPassDesc();
        mSegmentWork = false;
    }
    mNativeSlots[size_t(NativeStage::Compute)] = NativeSlots();
    mThreadgroupMemory = {};
    if (!openCompute())
    {
        closePass(mTracker.endComputePass(), "beginComputePass");
        return false;
    }
    return true;
}

bool MetalEncoder::openCompute()
{
    if (mCompute)
    {
        return true;
    }
    // Its own compute encoder on the frame's command buffer: the copies end first, the open debug groups go on in
    // the encoder (as open() does for a render encoder), and the queue barrier makes it see what the queue's earlier
    // work wrote (MTL4CommandEncoder.h:50-58).
    const std::string& name = mTracker.computePass().name;
    mDevice.endCopies();
    IdMTL4CommandBuffer buffer = mDevice.commandBuffer();
    // The pass timer's begin, on the command buffer before the encoder (MetalPassTimes).
    mComputeStamp = mDevice.passTimes().recording()
                        ? mDevice.passTimes().beginCompute(buffer, name, MetalPassTimes::Kind::Compute,
                                                           mDevice.lifecycle().currentFrame())
                        : MetalPassStamp();
    mDevice.endGroups();
    mCompute = [buffer computeCommandEncoder]; // MTL4CommandBuffer.h:130
    mDevice.beginGroups(mCompute);             // on the command buffer again when Metal gave no encoder
    if (!mCompute)
    {
        LL_WARNS("RHI") << "Metal gave no compute encoder for pass '" << name << "'" << LL_ENDL;
        return false;
    }
    mCompute.label = metalString(name);
    ++mComputeEncoders;
    mDevice.queueBarrier(mCompute);
    mComputeState = nil;
    if (makeTables("beginComputePass"))
    {
        [mCompute setArgumentTable:mTables[2]]; // MTL4ComputeCommandEncoder.h:661; snapshotted at each dispatch
    }
    // The table's places are written again for the new encoder, whatever the shadow held, and the pass's threadgroup
    // memory is set again.
    mShadows[2] = MetalTableShadow();
    for (U32 index = 0; index < NATIVE_THREADGROUP_SLOTS; ++index)
    {
        if (mThreadgroupMemory[index] != 0)
        {
            [mCompute setThreadgroupMemoryLength:mThreadgroupMemory[index] atIndex:index];
        }
    }
    return true;
}

void MetalEncoder::endComputePass()
{
    onRecordingThread("endComputePass");
    closePass(mTracker.endComputePass(), "endComputePass");
}

void MetalEncoder::closeCompute()
{
    if (!mCompute)
    {
        return;
    }
    // What the dispatches wrote comes before every stage of every later encoder on the queue, a split render pass's
    // vertex stage too (MTL4CommandEncoder.h:74-93), so later passes need no barrier of their own for it.
    [mCompute barrierAfterStages:MTLStageDispatch
               beforeQueueStages:MTLStageAll
               visibilityOptions:MTL4VisibilityOptionDevice];
    mDevice.endGroups();
    [mCompute endEncoding];
    mDevice.beginGroups(nil);
    mCompute = nil;
    mComputeState = nil;
    if (mComputeStamp.frame)
    {
        // The pass timer's end, on the command buffer after the encoder (MetalPassTimes).
        mDevice.passTimes().endCompute(mDevice.commandBuffer(), mComputeStamp);
        mComputeStamp = MetalPassStamp();
    }
}

void MetalEncoder::setComputePipeline(ComputePipeline* pipeline)
{
    onRecordingThread("setComputePipeline");
    // The state is set at the next dispatch, which waits for a pipeline still compiling.
    accept("setComputePipeline", mTracker.setComputePipeline(pipeline));
}

void MetalEncoder::setThreadgroupMemory(U32 index, U32 bytes)
{
    onRecordingThread("setThreadgroupMemory");
    if (!accept("setThreadgroupMemory", mTracker.setThreadgroupMemory(index, bytes)))
    {
        return;
    }
    mThreadgroupMemory[index] = bytes;
    if (mCompute)
    {
        [mCompute setThreadgroupMemoryLength:bytes atIndex:index]; // MTL4ComputeCommandEncoder.h:59
    }
}

void MetalEncoder::dispatchThreads(DispatchSize grid, DispatchSize threadsPerGroup)
{
    onRecordingThread("dispatchThreads");
    bool barrier = false;
    if (accept("dispatchThreads", mTracker.dispatch(grid, threadsPerGroup, barrier)))
    {
        encodeDispatch("dispatchThreads", grid, threadsPerGroup, false, barrier);
    }
}

void MetalEncoder::dispatchThreadgroups(DispatchSize groups, DispatchSize threadsPerGroup)
{
    onRecordingThread("dispatchThreadgroups");
    bool barrier = false;
    if (accept("dispatchThreadgroups", mTracker.dispatch(groups, threadsPerGroup, barrier)))
    {
        encodeDispatch("dispatchThreadgroups", groups, threadsPerGroup, true, barrier);
    }
}

void MetalEncoder::encodeDispatch(const char* call, DispatchSize size, DispatchSize threadsPerGroup, bool groups,
                                  bool barrier)
{
    // A submit inside the pass ended its encoder (splitPass); the next dispatch goes on in a new one, after the queue
    // barrier, which orders it after everything before.
    if (!mCompute && !openCompute())
    {
        return;
    }
    IdMTLComputePipelineState state = metalComputePipelineState(mTracker.computePipeline());
    if (!state)
    {
        LL_WARNS("RHI") << call << ": compute pipeline '" << mTracker.computePipeline()->desc().function
                        << "' failed to compile" << LL_ENDL;
        return;
    }
    if (!bindForDispatch(call))
    {
        return;
    }
    if (mComputeStamp.wantsProgram)
    {
        mDevice.passTimes().noteProgram(mComputeStamp, state.label);
    }
    if (state != mComputeState)
    {
        [mCompute setComputePipelineState:state]; // MTL4ComputeCommandEncoder.h:51
        mComputeState = state;
    }
    if (barrier)
    {
        // The dispatch reads or writes what an earlier dispatch of the pass wrote (MTL4CommandEncoder.h:113).
        [mCompute barrierAfterEncoderStages:MTLStageDispatch
                        beforeEncoderStages:MTLStageDispatch
                          visibilityOptions:MTL4VisibilityOptionDevice];
        ++mDispatchBarriers;
    }
    const MTLSize perGroup = MTLSizeMake(threadsPerGroup.x, threadsPerGroup.y, threadsPerGroup.z);
    if (groups)
    {
        [mCompute dispatchThreadgroups:MTLSizeMake(size.x, size.y, size.z) threadsPerThreadgroup:perGroup];
    }
    else
    {
        [mCompute dispatchThreads:MTLSizeMake(size.x, size.y, size.z) threadsPerThreadgroup:perGroup];
    }
    ++mNativeDispatches;
    markNativeWrites(NativeStage::Compute);
}

void MetalEncoder::markNativeWrites(NativeStage stage)
{
    for (const auto& [slot, binding] : mTracker.nativeTextures(stage))
    {
        if (binding.access == Access::Read)
        {
            continue;
        }
        Texture* texture = binding.texture.get();
        const auto levels = rangeLevels(binding.range, texture->desc().mipLevels);
        const U32 slices = metalSlices(texture->desc());
        for (U32 level = levels.first; level < levels.second; ++level)
        {
            for (U32 slice = 0; slice < slices; ++slice)
            {
                metalMarkWritten(texture, level, slice);
            }
        }
    }
}

void MetalEncoder::setNativeTexture(NativeStage stage, U32 slot, Texture* texture, Access access, TextureRange range)
{
    onRecordingThread("setNativeTexture");
    // The tracker holds the binding; the next draw or dispatch writes it into the stage's table.
    if (accept("setNativeTexture", mTracker.setNativeTexture(stage, slot, texture, access, range)) && texture
        && !mDepthClears.empty())
    {
        settleDepthClears(texture);
    }
}

void MetalEncoder::setNativeSampler(NativeStage stage, U32 slot, const SamplerDesc& sampling)
{
    onRecordingThread("setNativeSampler");
    if (accept("setNativeSampler", mTracker.setNativeSampler(stage, slot)))
    {
        mNativeSlots[size_t(stage)].samplers[slot] = sampling;
    }
}

void MetalEncoder::setNativeBytes(NativeStage stage, U32 slot, std::span<const std::byte> data)
{
    onRecordingThread("setNativeBytes");
    if (!accept("setNativeBytes", mTracker.setNativeBytes(stage, slot, data.size())))
    {
        clearNativeBytes(stage, slot); // refused: the slot is left empty, not with the bytes before
        return;
    }
    // A copy in the frame's ring, which lives for the frame slot, as the translated programs' default blocks.
    const MetalAllocation bytes = mDevice.frameAllocation(data.size());
    if (!bytes.buffer)
    {
        mNativeSlots[size_t(stage)].bytes[slot] = 0;
        return; // the ring wrote the WARN line
    }
    std::memcpy(bytes.cpu, data.data(), data.size());
    mNativeSlots[size_t(stage)].bytes[slot] = bytes.buffer.gpuAddress + bytes.offset;
}

void MetalEncoder::setNativeBuffer(NativeStage stage, U32 slot, Buffer* buffer, U64 offset, Access access)
{
    onRecordingThread("setNativeBuffer");
    // The buffer takes the slot; a refused one leaves it empty, as the tracker does.
    accept("setNativeBuffer", mTracker.setNativeBuffer(stage, slot, buffer, offset, access));
    clearNativeBytes(stage, slot);
}

void MetalEncoder::clearNativeBytes(NativeStage stage, U32 slot)
{
    if (size_t(stage) < mNativeSlots.size() && slot < NATIVE_BUFFER_SLOTS)
    {
        mNativeSlots[size_t(stage)].bytes[slot] = 0;
    }
}

bool MetalTimerQuery::ready() const
{
    const MetalTimerRun* run = mRun.get();
    if (!run || !run->ended)
    {
        return run && run->abandoned; // never begun or still active: not ready; abandoned: ready, 0 ns
    }
    if (!run->endWritten)
    {
        return true; // no result: ready, 0 ns
    }
    const MetalReadbackStamp& stamp = *run->endStamp;
    if (!stamp.serial || stamp.event.signaledValue < stamp.serial)
    {
        return false;
    }
    // As MetalQuery::ready: reported, failed or not (the event does not order the feedback handler).
    std::shared_ptr<MetalDevice::FeedbackState> feedback = mDevice.feedbackState();
    std::lock_guard<std::mutex> lock(feedback->mutex);
    return stamp.submission && stamp.submission->received;
}

U64 MetalTimerQuery::result(bool wait)
{
    MetalDevice::QueryStats& stats = mDevice.queryStats();
    const std::shared_ptr<MetalTimerRun> run = mRun;
    if (!run || !run->ended || !run->endWritten)
    {
        ++stats.timerInvalid; // never begun, still active, refused, abandoned or without a pair: 0 ns
        return 0;
    }
    if (!ready())
    {
        if (!wait)
        {
            return 0;
        }
        ++stats.timeWaited;
        if (!run->endStamp->serial)
        {
            mDevice.submitPending(); // llrhi.h: on Metal the recorded work is sent and waited for, as Readback::wait
        }
        const MetalReadbackStamp& stamp = *run->endStamp;
        if (!stamp.serial || ![stamp.event waitUntilSignaledValue:stamp.serial timeoutMS:METAL_WAIT_MS]
            || !mDevice.waitForFeedback(stamp.submission))
        {
            LL_WARNS("RHI") << "Time query: its command buffer was not sent, did not finish within " << METAL_WAIT_MS
                            << " ms or failed; it reads 0 ns" << LL_ENDL;
            ++stats.timerInvalid;
            return 0;
        }
    }
    // The pair holds this run's stamps only when both went in command buffers that completed without error. The
    // feedback queue reports in commit order, so the begin's submission, never after the end's, has reported too.
    bool failed = true;
    {
        std::shared_ptr<MetalDevice::FeedbackState> feedback = mDevice.feedbackState();
        std::lock_guard<std::mutex> lock(feedback->mutex);
        const MetalSubmission* begin = run->beginStamp->submission.get();
        const MetalSubmission* end = run->endStamp->submission.get();
        failed = !begin || !end || !begin->received || !end->received || begin->failed || end->failed;
    }
    std::array<U64, 2> ticks = run->kept;
    if (failed || (!run->captured && !mDevice.resolveTimestamps(run->pair, ticks)) || ticks[0] == 0
        || ticks[1] < ticks[0] || mDevice.timestampFrequency() == 0)
    {
        ++stats.timerInvalid;
        return 0;
    }
    const U64 ns = U64(double(ticks[1] - ticks[0]) * 1e9 / double(mDevice.timestampFrequency()) + 0.5);
    ++stats.timeRead;
    stats.timeZero += ns == 0 ? 1 : 0;
    return ns;
}

bool MetalQuery::ready() const
{
    const MetalQueryRun* run = mRun.get();
    if (!run || !run->ended)
    {
        return run && run->abandoned; // never begun or still active: not ready; abandoned: ready, visible
    }
    if (run->slot < 0 || run->uncounted || run->draws == 0)
    {
        return true; // no result: ready, visible
    }
    const MetalReadbackStamp& stamp = *run->stamp;
    if (!stamp.serial || stamp.event.signaledValue < stamp.serial)
    {
        return false;
    }
    // Reported, failed or not, as MetalReadback::ready (the event does not order the feedback handler).
    std::shared_ptr<MetalDevice::FeedbackState> feedback = mDevice.feedbackState();
    std::lock_guard<std::mutex> lock(feedback->mutex);
    return stamp.submission && stamp.submission->received;
}

U64 MetalQuery::result(bool wait)
{
    MetalDevice::QueryStats& stats = mDevice.queryStats();
    const std::shared_ptr<MetalQueryRun> run = mRun;
    if (!run || !run->ended || run->slot < 0 || run->uncounted || run->draws == 0)
    {
        // no result: visible, so a missing result never hides an object; with no draw (Metal refused them all, or none
        // came) GL would read 0, but a draw Metal refused in only one pass format would hide its group for good
        return 1;
    }
    if (!ready())
    {
        if (!wait)
        {
            return 1;
        }
        ++stats.waited;
        if (!run->stamp->serial)
        {
            mDevice.submitPending(); // llrhi.h: on Metal the recorded work is sent and waited for, as Readback::wait
        }
        const MetalReadbackStamp& stamp = *run->stamp;
        if (!stamp.serial || ![stamp.event waitUntilSignaledValue:stamp.serial timeoutMS:METAL_WAIT_MS]
            || !mDevice.waitForFeedback(stamp.submission))
        {
            LL_WARNS("RHI") << "Query: its command buffer was not sent, did not finish within " << METAL_WAIT_MS
                            << " ms or failed; it reads as visible" << LL_ENDL;
            return 1;
        }
    }
    {
        std::shared_ptr<MetalDevice::FeedbackState> feedback = mDevice.feedbackState();
        std::lock_guard<std::mutex> lock(feedback->mutex);
        if (run->stamp->submission && run->stamp->submission->failed)
        {
            return 1; // a failed command buffer wrote nothing reliable
        }
    }
    const U64 value = run->captured ? run->kept : static_cast<const U64*>(run->buffer.contents)[run->slot];
    ++stats.read;
    stats.zero += value == 0 ? 1 : 0;
    // Boolean mode promises only non-zero (MTLRenderCommandEncoder.h:38-42); 0 or 1 survives the callers' GLuint cast
    return mType == QueryType::AnySamplesPassed ? U64(value != 0) : value;
}

} // namespace LL::RHI

#endif // LL_RHI_METAL
