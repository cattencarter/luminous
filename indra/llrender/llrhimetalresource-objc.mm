/**
 * @file llrhimetalresource-objc.mm
 * @brief The Metal backend's buffers, textures, samplers and uploads
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

#include <algorithm>
#include <array>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstring>

#if LL_RHI_METAL

namespace LL::RHI
{

namespace
{
    /// What validate(TextureDesc) leaves to the backend: a format without a Metal format (no unit
    /// yet), a multisampled texture that is read back or sampled (not supported yet), or null. validate() already makes
    /// a multisampled texture a 2D render target with one mip level and a power of two up to maxSamples samples
    /// (llrhi.cpp:684-694). API Validation takes a cube array of one cube (a run-time probe).
    const char* textureRefusal(const TextureDesc& desc)
    {
        if (metalPixelFormat(desc.format) == MTLPixelFormatInvalid)
        {
            return "the format has no Metal format (no unit yet)";
        }
        // A multisampled texture is drawn into and resolved by copyTexture. A buffer copy from it is a validation
        // error (a run-time probe: "destinationBytesPerRow(64) must be >= (256)"); neither it nor sampling one is
        // supported yet.
        if (desc.samples > 1 && (desc.usage & (TEXTURE_READBACK | TEXTURE_SAMPLED)))
        {
            return "multisampled textures that are read back or sampled come with P3-08";
        }
        return nullptr;
    }

    MTLTextureSwizzle metalSwizzle(Swizzle swizzle)
    {
        switch (swizzle)
        {
        case Swizzle::R:    return MTLTextureSwizzleRed;
        case Swizzle::G:    return MTLTextureSwizzleGreen;
        case Swizzle::B:    return MTLTextureSwizzleBlue;
        case Swizzle::A:    return MTLTextureSwizzleAlpha;
        case Swizzle::Zero: return MTLTextureSwizzleZero;
        case Swizzle::One:  return MTLTextureSwizzleOne;
        }
        return MTLTextureSwizzleZero;
    }

    U32 componentCount(PixelLayout::Components components)
    {
        switch (components)
        {
        case PixelLayout::Components::R:     return 1;
        case PixelLayout::Components::RG:    return 2;
        case PixelLayout::Components::RGB:   return 3;
        case PixelLayout::Components::RGBA:  return 4;
        case PixelLayout::Components::BGRA:  return 4;
        case PixelLayout::Components::Depth: return 1;
        }
        return 0;
    }

    /// Bytes of one component; U32_8_8_8_8_Rev's are its four bytes in memory order.
    U32 componentBytes(PixelLayout::Type type)
    {
        switch (type)
        {
        case PixelLayout::Type::U8:              return 1;
        case PixelLayout::Type::U16:             return 2;
        case PixelLayout::Type::F16:             return 2;
        case PixelLayout::Type::U32:             return 4;
        case PixelLayout::Type::F32:             return 4;
        case PixelLayout::Type::U32_8_8_8_8_Rev: return 1;
        }
        return 0;
    }

    /// The largest value of a component type, alpha 1 for GL: 0xFF, 0xFFFF, 1.0f and half 0x3C00.
    U32 alphaOne(PixelLayout::Type type)
    {
        switch (type)
        {
        case PixelLayout::Type::U8:              return 0xFF;
        case PixelLayout::Type::U16:             return 0xFFFF;
        case PixelLayout::Type::F16:             return 0x3C00;
        case PixelLayout::Type::F32:             return 0x3F800000;
        case PixelLayout::Type::U32:             return 0xFFFFFFFF;
        case PixelLayout::Type::U32_8_8_8_8_Rev: return 0xFF;
        }
        return 0;
    }

    /// A float's half as Apple's GL makes it for GL_RGB16F from GL_FLOAT: the mantissa truncated
    /// toward zero, subnormal halves kept, Inf where the exponent is above the largest half's
    /// (65520 is 65504, 70000 is Inf). A run-time probe on Apple's GL checked every value of
    /// both noise maps and the constructed ties, subnormals and overflows (llrhiformatcases.h).
    U32 halfFromFloat(U32 bits)
    {
        const U32 sign = (bits >> 16) & 0x8000;
        const U32 exponent = (bits >> 23) & 0xFF;
        const U32 mantissa = bits & 0x7FFFFF;
        if (exponent == 0xFF)
        {
            return sign | 0x7C00 | (mantissa ? 0x200 | (mantissa >> 13) : 0);
        }
        const S32 halfExponent = S32(exponent) - 127 + 15;
        if (halfExponent >= 31)
        {
            return sign | 0x7C00;
        }
        if (halfExponent >= 1)
        {
            return sign | (U32(halfExponent) << 10) | (mantissa >> 13);
        }
        // A subnormal half: the value in units of 2^-24, truncated.
        const U32 shift = U32(126 - S32(exponent));
        if (exponent == 0 || shift > 24)
        {
            return sign;
        }
        return sign | ((mantissa | 0x800000) >> shift);
    }

    U32 readComponent(const std::byte* at, U32 bytes, bool swap)
    {
        std::array<std::byte, 4> raw{};
        memcpy(raw.data(), at, bytes);
        if (swap)
        {
            std::reverse(raw.begin(), raw.begin() + bytes);
        }
        U32 value = 0;
        memcpy(&value, raw.data(), bytes); // the Mac's CPUs are little-endian
        return value;
    }
    MTLSamplerAddressMode addressMode(AddressMode mode)
    {
        switch (mode)
        {
        case AddressMode::Repeat:         return MTLSamplerAddressModeRepeat;
        case AddressMode::MirroredRepeat: return MTLSamplerAddressModeMirrorRepeat;
        case AddressMode::ClampToEdge:    return MTLSamplerAddressModeClampToEdge;
        }
        return MTLSamplerAddressModeRepeat;
    }
}

/// The body is here, so no other file depends on it.
class API_AVAILABLE(macos(26.0)) MetalTexture final : public Texture
{
public:
    MetalTexture(MetalDevice& device, const TextureDesc& desc, IdMTLTexture texture)
    :   mDevice(device), mDesc(desc), mTexture(texture), mWritten(size_t(desc.mipLevels) * metalSlices(desc))
    {
    }

    ~MetalTexture() override
    {
        // The texture leaves the residency set once the GPU has finished the frames that used it, and the main
        // encoder's shadows forget it and its level views before they are freed and their MTLResourceIDs can come
        // back (forgetTexture).
        MetalDevice* device = &mDevice;
        IdMTLTexture texture = mTexture;
        std::vector<IdMTLTexture> views;
        for (const auto& [key, view] : mViews)
        {
            views.push_back(view);
        }
        mDevice.lifecycle().release([device, texture, views]()
        {
            device->residency().retire(texture);
            device->metalEncoder().forgetTexture(texture.gpuResourceID);
            for (IdMTLTexture view : views)
            {
                device->metalEncoder().forgetTexture(view.gpuResourceID);
            }
        });
    }

    /// A view of levels [first, end) and every slice, made at the first ask and kept with the texture; the texture
    /// itself when the range is all of it. Main thread, as the encoder.
    IdMTLTexture levelView(U32 first, U32 end)
    {
        if (first == 0 && end == mDesc.mipLevels)
        {
            return mTexture;
        }
        const U64 key = (U64(first) << 32) | end;
        const auto found = mViews.find(key);
        if (found != mViews.end())
        {
            return found->second;
        }
        // MTLTexture.h's newTextureViewWithPixelFormat:textureType:levels:slices:, a view sharing the texture's
        // storage, so its residency is the texture's (MetalResidency::containsTexture).
        IdMTLTexture view = [mTexture newTextureViewWithPixelFormat:mTexture.pixelFormat
                                                        textureType:mTexture.textureType
                                                             levels:NSMakeRange(first, end - first)
                                                             slices:NSMakeRange(0, mTexture.textureType == MTLTextureType3D
                                                                                        ? 1
                                                                                        : metalSlices(mDesc))];
        if (view)
        {
            mViews.emplace(key, view);
        }
        return view;
    }

    const TextureDesc& desc() const override { return mDesc; }
    IdMTLTexture texture() const { return mTexture; }

    /// One mark per level and slice (the level's slices one after another).
    bool written(U32 level, U32 layer) const
    {
        const size_t at = place(level, layer);
        return at < mWritten.size() && mWritten[at];
    }
    void markWritten(U32 level, U32 layer)
    {
        const size_t at = place(level, layer);
        if (at < mWritten.size())
        {
            mWritten[at] = true;
        }
    }
    /// Unwritten again, as at creation.
    void forgetWritten(U32 level, U32 layer)
    {
        const size_t at = place(level, layer);
        if (at < mWritten.size())
        {
            mWritten[at] = false;
        }
    }

private:
    size_t place(U32 level, U32 layer) const
    {
        const size_t slices = mDesc.mipLevels ? mWritten.size() / mDesc.mipLevels : 0;
        return level < mDesc.mipLevels && layer < slices ? size_t(level) * slices + layer : mWritten.size();
    }

    MetalDevice& mDevice;
    const TextureDesc mDesc;
    IdMTLTexture mTexture;
    std::vector<bool> mWritten;
    std::unordered_map<U64, IdMTLTexture> mViews; ///< levelView's, by (first << 32 | end)
};

IdMTLTexture metalTexture(Texture* texture)
{
    return texture ? static_cast<MetalTexture*>(texture)->texture() : nil;
}

IdMTLTexture metalLevelView(Texture* texture, U32 first, U32 end)
{
    return texture ? static_cast<MetalTexture*>(texture)->levelView(first, end) : nil;
}

U32 metalSlices(const TextureDesc& desc)
{
    switch (desc.type)
    {
    case TextureType::Tex2D:     return 1;
    case TextureType::Cube:      return 6;
    case TextureType::CubeArray: return 6 * desc.layers;
    case TextureType::Tex3D:     return desc.layers; // one mark per depth slice
    }
    return 1;
}

bool metalWritten(Texture* texture, U32 level, U32 layer)
{
    return texture && static_cast<MetalTexture*>(texture)->written(level, layer);
}

void metalMarkWritten(Texture* texture, U32 level, U32 layer)
{
    if (texture)
    {
        static_cast<MetalTexture*>(texture)->markWritten(level, layer);
    }
}

void metalForgetWritten(Texture* texture, U32 level, U32 layer)
{
    if (texture)
    {
        static_cast<MetalTexture*>(texture)->forgetWritten(level, layer);
    }
}

/// A buffer whose memory is a Shared buffer of its own or a pool block (no GL name).
/// A dynamic buffer's update replaces the memory; the old goes when the GPU is done with it.
class API_AVAILABLE(macos(26.0)) MetalBuffer final : public Buffer
{
public:
    MetalBuffer(MetalDevice& device, const BufferDesc& desc, const MetalAllocation& memory)
    :   mDevice(device), mDesc(desc), mCurrent(memory)
    {
    }

    ~MetalBuffer() override
    {
        if (!mIndexCopy.empty())
        {
            mDevice.noteIndexCopy(-S64(mIndexCopy.size()));
        }
        mDevice.releaseBufferMemory(mDesc, mCurrent);
    }

    const BufferDesc& desc() const override { return mDesc; }
    const MetalAllocation& current() const { return mCurrent; }
    /// A static index buffer's bytes as GL has them, for drawIndexed's check and its
    /// fans, strips and loops; empty for the other buffers (metalIndexBytes).
    std::span<const std::byte> indexCopy() const { return mIndexCopy; }
    std::vector<std::byte>& indexCopy() { return mIndexCopy; }
    /// An index buffer was given a 16-bit 0xFFFF at an even offset (sticky; metalIndexHoldsRestart).
    bool holdsRestart() const { return mHoldsRestart; }
    void noteRestart(U64 offset, std::span<const std::byte> bytes)
    {
        for (size_t i = offset % 2; !mHoldsRestart && i + 1 < bytes.size(); i += 2)
        {
            mHoldsRestart = bytes[i] == std::byte(0xFF) && bytes[i + 1] == std::byte(0xFF);
        }
    }
    /// The new memory is current; the old goes to FrameLifecycle::release.
    void replace(const MetalAllocation& memory)
    {
        mDevice.releaseBufferMemory(mDesc, mCurrent);
        mCurrent = memory;
    }

private:
    MetalDevice& mDevice;
    const BufferDesc mDesc;
    MetalAllocation mCurrent;
    std::vector<std::byte> mIndexCopy;
    bool mHoldsRestart = false;
};

IdMTLBuffer metalBuffer(Buffer* buffer)
{
    return metalAllocation(buffer).buffer;
}

MetalAllocation metalAllocation(Buffer* buffer)
{
    return buffer ? static_cast<MetalBuffer*>(buffer)->current() : MetalAllocation();
}

std::span<const std::byte> metalIndexBytes(Buffer* buffer)
{
    if (!buffer)
    {
        return {};
    }
    const MetalBuffer& metal = *static_cast<const MetalBuffer*>(buffer);
    if (metal.desc().dynamic)
    {
        // An update gives new memory with every byte at the call.
        return { metal.current().cpu, size_t(metal.desc().size) };
    }
    return metal.indexCopy();
}

bool metalIndexHoldsRestart(Buffer* buffer)
{
    return buffer && static_cast<const MetalBuffer*>(buffer)->holdsRestart();
}

MTLResourceOptions metalBufferStorage(const BufferDesc& desc)
{
    // Shared for every usage: Apple silicon's memory is unified, and a buffer the CPU
    // writes at an update and the GPU reads needs no copy. Not write-combined, which reads poorly
    // (MTLResource.h:116).
    return MTLResourceStorageModeShared | MTLResourceCPUCacheModeDefaultCache;
}

Ref<Buffer> createMetalBuffer(MetalDevice& device, const BufferDesc& desc, std::span<const std::byte> initial,
                              std::string_view debugName)
{
    std::string why = validate(desc, initial.size(), &device.capabilities());
    if (!why.empty())
    {
        warnCreateFailed("buffer", debugName, why);
        return nullptr;
    }
    // A pool block for a small dynamic buffer, otherwise a buffer of its own, resident from
    // creation; the data at creation goes into this first memory.
    const MetalAllocation memory = device.newBufferMemory(desc, debugName);
    if (!memory.buffer)
    {
        warnCreateFailed("buffer", debugName, "Metal gave no buffer");
        return nullptr;
    }
    if (!initial.empty())
    {
        memcpy(memory.cpu, initial.data(), initial.size());
    }
    if (metalPooled(desc) && initial.size() < desc.size)
    {
        // A reused block holds an earlier buffer's bytes; a new buffer of its own is zeros.
        memset(memory.cpu + initial.size(), 0, desc.size - initial.size());
    }
    Ref<MetalBuffer> result = new MetalBuffer(device, desc, memory);
    result->setDebugName(debugName);
    if (desc.usage & BUFFER_INDEX)
    {
        result->noteRestart(0, initial); // where its indices are written
    }
    if (!desc.dynamic && (desc.usage & BUFFER_INDEX))
    {
        // The bytes at creation, the rest zeros as in the buffer's own memory. Kept with the
        // debug setting or without: an indexed fan, strip or loop reads it.
        std::vector<std::byte>& copy = result->indexCopy();
        copy.assign(size_t(desc.size), std::byte(0));
        std::copy(initial.begin(), initial.end(), copy.begin());
        device.noteIndexCopy(S64(copy.size())); // the memory number
    }
    return result;
}

void metalUpdateBuffer(MetalDevice& device, Buffer* buffer, U64 offset, std::span<const std::byte> data)
{
    if (!buffer || data.empty())
    {
        return;
    }
    MetalBuffer& metal = *static_cast<MetalBuffer*>(buffer);
    const BufferDesc& desc = metal.desc();
    const MetalAllocation current = metal.current();
    if (desc.usage & BUFFER_INDEX)
    {
        metal.noteRestart(offset, data); // where its indices are written
    }
    if (desc.dynamic)
    {
        // New memory, so draws recorded before the call keep reading the old bytes; the bytes
        // outside the range come along on the CPU.
        const MetalAllocation next = device.newBufferMemory(desc, buffer->debugName());
        if (!next.buffer)
        {
            return; // newBufferMemory wrote the WARN line
        }
        if (offset > 0)
        {
            memcpy(next.cpu, current.cpu, offset);
        }
        memcpy(next.cpu + offset, data.data(), data.size());
        const U64 end = offset + data.size();
        if (end < desc.size)
        {
            memcpy(next.cpu + end, current.cpu + end, desc.size - end);
        }
        metal.replace(next);
        return;
    }
    // GL reads the bytes at the call, so they go to the ring now; the GPU copies them on an
    // encoder of its own after the queue barrier, before the next pass uses the buffer
    // (MTL4ComputeCommandEncoder.h:355). In a pass the encoder has broken it first.
    const MetalAllocation staging = device.frameAllocation(data.size());
    if (!staging.buffer)
    {
        return;
    }
    memcpy(staging.cpu, data.data(), data.size());
    IdMTL4ComputeCommandEncoder copies = device.beginUploadCopies();
    if (!copies)
    {
        return;
    }
    [copies copyFromBuffer:staging.buffer
              sourceOffset:staging.offset
                  toBuffer:current.buffer
         destinationOffset:current.offset + offset
                      size:data.size()];
    device.endCopies();
    // The index copy has the bytes at the call, as the GPU's will once the copy has run.
    std::vector<std::byte>& copy = metal.indexCopy();
    if (offset + data.size() <= copy.size())
    {
        std::copy(data.begin(), data.end(), copy.begin() + offset);
    }
}

/// An immutable sampler state, which Metal 4's command buffers do not keep.
class API_AVAILABLE(macos(26.0)) MetalSampler final : public Sampler
{
public:
    MetalSampler(FrameLifecycle& lifecycle, const SamplerDesc& desc, IdMTLSamplerState state)
    :   mLifecycle(lifecycle), mDesc(desc), mState(state)
    {
    }

    ~MetalSampler() override
    {
        // The state lives until the frames that used it have finished.
        IdMTLSamplerState state = mState;
        mLifecycle.release([state]() mutable { state = nil; });
    }

    const SamplerDesc& desc() const override { return mDesc; }
    IdMTLSamplerState state() const { return mState; }

private:
    FrameLifecycle& mLifecycle;
    const SamplerDesc mDesc;
    IdMTLSamplerState mState;
};

IdMTLSamplerState metalSampler(Sampler* sampler)
{
    return sampler ? static_cast<MetalSampler*>(sampler)->state() : nil;
}

MTLSamplerDescriptor* metalSamplerDescriptor(const SamplerDesc& desc, bool anisotropic)
{
    // LLTexUnit::toGLSampling's filters (llrender.cpp:1755-1781) on MTLSampler.h:114-128.
    // The mag filter is magFilter's, or the filter's; the min filter nearest only for Point; the mip
    // filter linear for Trilinear and Anisotropic, nearest for Point and Bilinear, none without mips.
    MTLSamplerDescriptor* metal = [MTLSamplerDescriptor new];
    const bool point = desc.filter == Filter::Point;
    metal.magFilter = desc.magFilter == MagFilter::Nearest  ? MTLSamplerMinMagFilterNearest
                      : desc.magFilter == MagFilter::Linear ? MTLSamplerMinMagFilterLinear
                      : point                               ? MTLSamplerMinMagFilterNearest
                                                            : MTLSamplerMinMagFilterLinear;
    metal.minFilter = point ? MTLSamplerMinMagFilterNearest : MTLSamplerMinMagFilterLinear;
    metal.mipFilter = !desc.mipmapped                    ? MTLSamplerMipFilterNotMipmapped
                      : desc.filter >= Filter::Trilinear ? MTLSamplerMipFilterLinear
                                                         : MTLSamplerMipFilterNearest;
    // sampler2DShadow's comparing sampler (GL_TEXTURE_COMPARE_MODE with its function, llrhigl.cpp:2380-2384) is
    // Metal's compareFunction, which MSL's sample_compare uses (MTLSampler.h); a linear filter is Metal's own PCF, not
    // GL's weights (GL's weights are not emulated).
    if (desc.compare)
    {
        metal.compareFunction = metalCompareFunction(desc.compareFunc);
    }
    metal.sAddressMode = addressMode(desc.addressU);
    metal.tAddressMode = addressMode(desc.addressV);
    metal.rAddressMode = addressMode(desc.addressW);
    // gl_texture_filter (llrender.cpp:383-395) gives GL_TEXTURE_MAX_ANISOTROPY for every filter on a device
    // with anisotropic filtering. Metal takes whole samples (MTLSampler.h:130-135): the rounded value, 1 for
    // NaN and values below 1 (validate(SamplerDesc) refuses them), at most the device's.
    const F32 anisotropy = desc.maxAnisotropy;
    metal.maxAnisotropy =
        anisotropic && anisotropy >= 1.f ? NSUInteger(std::lround(std::min(anisotropy, METAL_MAX_ANISOTROPY))) : 1;
    // GL's whole mip chain (MTLSampler.h:178-190).
    metal.lodMinClamp = 0.f;
    metal.lodMaxClamp = FLT_MAX;
    // Argument tables bind it by its gpuResourceID (MTLSampler.h:213-216, 246-249).
    metal.supportArgumentBuffers = YES;
    return metal;
}

Ref<Sampler> createMetalSampler(MetalDevice& device, const SamplerDesc& desc)
{
    // Every filter, mag filter, anisotropy and compare function is ported.
    IdMTLSamplerState state = [device.mtlDevice()
        newSamplerStateWithDescriptor:metalSamplerDescriptor(desc, device.capabilities().anisotropicFiltering)];
    if (!state)
    {
        warnCreateFailed("sampler", {}, "Metal gave none");
        return nullptr;
    }
    return new MetalSampler(device.lifecycle(), desc, state);
}

MetalTextureStorage metalTextureStorage(const TextureDesc& desc)
{
    // Private: the GPU's own memory, filled by copies (MTLResource.h:84-87). Memoryless only when asked
    // (TEXTURE_MEMORYLESS): it keeps nothing past a pass (MTLResource.h:75-79).
    MetalTextureStorage storage;
    storage.mode = (desc.usage & TEXTURE_MEMORYLESS) ? MTLStorageModeMemoryless : MTLStorageModePrivate;
    if (desc.usage & TEXTURE_SAMPLED)
    {
        storage.usage |= MTLTextureUsageShaderRead;
    }
    if (desc.usage & TEXTURE_RENDER_TARGET)
    {
        storage.usage |= MTLTextureUsageRenderTarget;
    }
    if (desc.usage & TEXTURE_STORAGE)
    {
        // Written by native programs, through views of single levels (MTLTexture.h's MTLTextureUsagePixelFormatView
        // is not needed: the views keep the texture's format).
        storage.usage |= MTLTextureUsageShaderWrite;
    }
    // TEXTURE_READBACK: a copy to a buffer needs no usage.
    return storage;
}

std::optional<PixelLayout> metalReadbackLayout(Format format)
{
    using Components = PixelLayout::Components;
    using Type = PixelLayout::Type;
    const auto layout = [](Components components, Type type)
    {
        PixelLayout result;
        result.components = components;
        result.type = type;
        return std::optional<PixelLayout>(result);
    };
    switch (format)
    {
    case Format::R8Unorm:
    case Format::R8Uint:                return layout(Components::R, Type::U8);
    case Format::RG8Unorm:              return layout(Components::RG, Type::U8);
    case Format::RGBA8Unorm:
    case Format::RGBA8Unorm_sRGB:       return layout(Components::RGBA, Type::U8);
    case Format::R16Float:              return layout(Components::R, Type::F16);
    case Format::RG16Float:             return layout(Components::RG, Type::F16);
    case Format::RGBA16Float:           return layout(Components::RGBA, Type::F16);
    case Format::RGBA16Unorm:           return layout(Components::RGBA, Type::U16);
    case Format::R32Float:              return layout(Components::R, Type::F32);
    case Format::RG32Float:             return layout(Components::RG, Type::F32);
    case Format::RGBA32Float:           return layout(Components::RGBA, Type::F32);
    case Format::R16Uint:               return layout(Components::R, Type::U16);
    case Format::R32Uint:               return layout(Components::R, Type::U32);
    case Format::Depth32Float:
    case Format::Depth32Float_Stencil8: return layout(Components::Depth, Type::F32);
    default:                            return std::nullopt;
    }
}

namespace
{
    /// What one upload's conversion does, decided once from the description, the layout and the source's
    /// size, with every refusal of the conversion table and its WARN line: the texels' rows and bytes, and which of the
    /// copies writes them. The fast paths are byte for byte the general loop's result for their pairs (a unit test
    /// checks it): U8 components are one byte each, so swapBytes changes nothing there, as readComponent's swap of one
    /// byte does nothing.
    struct UploadPlan
    {
        enum class Kind
        {
            Blocks,  ///< whole 4x4 blocks, copied as they are
            Rows,    ///< (a) U8 RGBA into U8 RGBA texels with the source's alpha: rows copied
            Swizzle, ///< (b) U8 BGRA into U8 RGBA texels: one 32-bit word per pixel, R and B swapped
            Alpha,   ///< (c) U8 RGBA into U8 RGBA texels with alpha forced to 1
            General, ///< every other pair: per component (readComponent), the table's conversions
        };
        Kind kind = Kind::General;
        U32 rowBytes = 0; ///< 0: refused
        U64 bytes = 0;
        U32 rowLength = 0;
        U32 pixelBytes = 0;
        bool forceAlpha = false;
    };

    UploadPlan uploadPlan(const TextureDesc& desc, const PixelLayout& layout, size_t srcSize, const Region& region)
    {
        UploadPlan plan;
        if (const U32 block = blockBytes(desc.format))
        {
            // Whole 4x4 blocks in rows of blocks; the layout is ignored (validateUpload).
            plan.kind = UploadPlan::Kind::Blocks;
            const U32 rowBytes = (region.width + 3) / 4 * block;
            const U64 bytes = U64(rowBytes) * ((region.height + 3) / 4);
            if (srcSize >= bytes)
            {
                plan.rowBytes = rowBytes;
                plan.bytes = bytes;
            }
            return plan;
        }
        using Components = PixelLayout::Components;
        using Type = PixelLayout::Type;
        const std::optional<PixelLayout> texel = metalReadbackLayout(desc.format);
        const U32 from = componentCount(layout.components);
        const U32 to = texel ? componentCount(texel->components) : 0;
        const bool rev = layout.type == Type::U32_8_8_8_8_Rev;
        // The pairs: the texel's own type, U32_8_8_8_8_Rev as bytes into U8 texels and F32 into F16; the
        // texel's components, RGB into RGBA and BGRA into RGBA; depth only into depth.
        const bool types = texel
                           && (layout.type == texel->type || (rev && texel->type == Type::U8)
                               || (layout.type == Type::F32 && texel->type == Type::F16));
        const bool components = texel
                                && ((layout.components == Components::Depth) == (texel->components == Components::Depth))
                                && (from == to || (from == 3 && to == 4));
        if (!types || !components || region.width == 0 || region.height == 0)
        {
            LL_WARNS("RHI") << "uploadTexture: no conversion of components " << U32(layout.components) << " and type "
                            << U32(layout.type) << " to format " << U32(desc.format) << " (no unit yet)" << LL_ENDL;
            return plan;
        }
        plan.pixelBytes = layout.bytesPerPixel();
        plan.rowLength = layout.rowLength != 0 ? layout.rowLength : region.width;
        const U64 needed = (U64(plan.rowLength) * (region.height - 1) + region.width) * plan.pixelBytes;
        if (srcSize < needed)
        {
            LL_WARNS("RHI") << "uploadTexture: " << srcSize << " bytes for a region that needs " << needed << LL_ENDL;
            return plan;
        }
        // GL keeps no alpha for a texture whose GL format has none, so four components get alpha 1.
        plan.forceAlpha = to == 4 && (from == 3 || !hasAlphaInGLFormat(desc.glFormat));
        plan.rowBytes = region.width * to * componentBytes(texel->type);
        plan.bytes = U64(plan.rowBytes) * region.height;
        if (layout.type == Type::U8 && texel->type == Type::U8 && texel->components == Components::RGBA)
        {
            if (layout.components == Components::BGRA)
            {
                plan.kind = UploadPlan::Kind::Swizzle;
            }
            else if (layout.components == Components::RGBA)
            {
                plan.kind = plan.forceAlpha ? UploadPlan::Kind::Alpha : UploadPlan::Kind::Rows;
            }
        }
        return plan;
    }

    /// The general loop, straight into dst: every byte of each texel is written.
    void convertGeneral(const TextureDesc& desc, const PixelLayout& layout, const std::byte* src, const Region& region,
                        const UploadPlan& plan, std::byte* dst)
    {
        using Type = PixelLayout::Type;
        const PixelLayout texel = *metalReadbackLayout(desc.format);
        const U32 from = componentCount(layout.components);
        const U32 to = componentCount(texel.components);
        const bool rev = layout.type == Type::U32_8_8_8_8_Rev;
        const bool f16 = layout.type == Type::F32 && texel.type == Type::F16;
        const bool bgra = layout.components == PixelLayout::Components::BGRA;
        const U32 fromBytes = componentBytes(layout.type);
        const U32 toBytes = componentBytes(texel.type);
        const U32 one = alphaOne(texel.type);
        for (U32 y = 0; y < region.height; ++y)
        {
            for (U32 x = 0; x < region.width; ++x)
            {
                const std::byte* pixel = src + (U64(y) * plan.rowLength + x) * plan.pixelBytes;
                std::array<std::byte, 4> word{};
                if (rev)
                {
                    // One 4-byte word: GL_UNPACK_SWAP_BYTES swaps it whole, then its bytes are the components.
                    memcpy(word.data(), pixel, 4);
                    if (layout.swapBytes)
                    {
                        std::reverse(word.begin(), word.end());
                    }
                    pixel = word.data();
                }
                std::array<U32, 4> values{};
                for (U32 c = 0; c < from; ++c)
                {
                    const U32 value = readComponent(pixel + c * fromBytes, fromBytes, layout.swapBytes && !rev);
                    values[c] = f16 ? halfFromFloat(value) : value;
                }
                if (bgra)
                {
                    std::swap(values[0], values[2]);
                }
                if (plan.forceAlpha)
                {
                    values[3] = one;
                }
                std::byte* out = dst + size_t(y) * plan.rowBytes + size_t(x) * to * toBytes;
                for (U32 c = 0; c < to; ++c)
                {
                    memcpy(out + c * toBytes, &values[c], toBytes);
                }
            }
        }
    }

    /// (b) and (c): one little-endian word per pixel, [B, G, R, A] in memory being B | G << 8 | R << 16 | A << 24; a
    /// loop of shifts and masks, which clang vectorises.
    void convertWords(const std::byte* src, const Region& region, const UploadPlan& plan, std::byte* dst, bool swizzle)
    {
        const U32 alpha = plan.forceAlpha ? 0xFF000000u : 0u;
        for (U32 y = 0; y < region.height; ++y)
        {
            const std::byte* in = src + U64(y) * plan.rowLength * 4;
            std::byte* out = dst + size_t(y) * plan.rowBytes;
            for (U32 x = 0; x < region.width; ++x)
            {
                U32 word;
                memcpy(&word, in + size_t(x) * 4, 4);
                if (swizzle)
                {
                    word = (word & 0xFF00FF00u) | ((word >> 16) & 0xFFu) | ((word & 0xFFu) << 16);
                }
                word |= alpha;
                memcpy(out + size_t(x) * 4, &word, 4);
            }
        }
    }
}

MetalUploadSize metalUploadSize(const TextureDesc& desc, const PixelLayout& layout, size_t srcSize, const Region& region)
{
    const UploadPlan plan = uploadPlan(desc, layout, srcSize, region);
    return { plan.rowBytes, plan.rowBytes ? plan.bytes : 0 };
}

U32 metalConvertUploadInto(const TextureDesc& desc, const PixelLayout& layout, std::span<const std::byte> src,
                           const Region& region, std::byte* dst, U64 dstBytes)
{
    const UploadPlan plan = uploadPlan(desc, layout, src.size(), region);
    if (plan.rowBytes == 0 || !dst || dstBytes < plan.bytes)
    {
        return 0;
    }
    switch (plan.kind)
    {
    case UploadPlan::Kind::Blocks:
        memcpy(dst, src.data(), size_t(plan.bytes));
        break;
    case UploadPlan::Kind::Rows:
        if (plan.rowLength == region.width)
        {
            memcpy(dst, src.data(), size_t(plan.bytes));
        }
        else
        {
            for (U32 y = 0; y < region.height; ++y)
            {
                memcpy(dst + size_t(y) * plan.rowBytes, src.data() + U64(y) * plan.rowLength * 4, plan.rowBytes);
            }
        }
        break;
    case UploadPlan::Kind::Swizzle:
        convertWords(src.data(), region, plan, dst, true);
        break;
    case UploadPlan::Kind::Alpha:
        convertWords(src.data(), region, plan, dst, false);
        break;
    case UploadPlan::Kind::General:
        convertGeneral(desc, layout, src.data(), region, plan, dst);
        break;
    }
    return plan.rowBytes;
}

U32 metalConvertUpload(const TextureDesc& desc, const PixelLayout& layout, std::span<const std::byte> src,
                       const Region& region, std::vector<std::byte>& dst)
{
    // The vector's form of metalConvertUploadInto, for the tests and their general reference
    const MetalUploadSize size = metalUploadSize(desc, layout, src.size(), region);
    if (size.rowBytes == 0)
    {
        return 0;
    }
    dst.resize(size_t(size.bytes));
    return metalConvertUploadInto(desc, layout, src, region, dst.data(), size.bytes);
}

U32 metalConvertUploadGeneral(const TextureDesc& desc, const PixelLayout& layout, std::span<const std::byte> src,
                              const Region& region, std::vector<std::byte>& dst)
{
    UploadPlan plan = uploadPlan(desc, layout, src.size(), region);
    if (plan.rowBytes == 0)
    {
        return 0;
    }
    if (plan.kind == UploadPlan::Kind::Blocks)
    {
        dst.assign(src.begin(), src.begin() + std::ptrdiff_t(plan.bytes));
        return plan.rowBytes;
    }
    dst.assign(size_t(plan.bytes), std::byte{ 0 });
    convertGeneral(desc, layout, src.data(), region, plan, dst.data());
    return plan.rowBytes;
}

Ref<Texture> createMetalTexture(MetalDevice& device, const TextureDesc& desc, std::string_view debugName)
{
    std::string why = validate(desc, device.capabilities());
    if (why.empty())
    {
        const char* refusal = textureRefusal(desc);
        why = refusal ? refusal : "";
    }
    if (!why.empty())
    {
        warnCreateFailed("texture", debugName, why);
        return nullptr;
    }

    // The facade's own GL format is the GL backend's, but for whether it has alpha.
    const MTLPixelFormat pixelFormat = metalPixelFormat(desc.format);
    MTLTextureDescriptor* descriptor = nil;
    if (desc.type == TextureType::Tex2D)
    {
        descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pixelFormat
                                                                        width:desc.width
                                                                       height:desc.height
                                                                    mipmapped:NO];
    }
    else if (desc.type == TextureType::Tex3D)
    {
        // A volume: layers is its depth; validate() made it one level, sampled only (MTLTexture.h's MTLTextureType3D).
        descriptor = [MTLTextureDescriptor new];
        descriptor.textureType = MTLTextureType3D;
        descriptor.pixelFormat = pixelFormat;
        descriptor.width = desc.width;
        descriptor.height = desc.height;
        descriptor.depth = desc.layers;
    }
    else
    {
        // Slice = face in GL's order (+X, -X, +Y, -Y, +Z, -Z); a cube array's slice is cube * 6 + face.
        descriptor = [MTLTextureDescriptor textureCubeDescriptorWithPixelFormat:pixelFormat size:desc.width mipmapped:NO];
        if (desc.type == TextureType::CubeArray)
        {
            descriptor.textureType = MTLTextureTypeCubeArray;
            descriptor.arrayLength = desc.layers;
        }
    }
    descriptor.mipmapLevelCount = desc.mipLevels;
    if (desc.samples > 1)
    {
        // validate() made it 2D with one level; a multisample texture (MTLTexture.h:29, 177-181), private
        // and a render target only (textureRefusal), resolved by copyTexture.
        descriptor.textureType = MTLTextureType2DMultisample;
        descriptor.sampleCount = desc.samples;
    }
    // GL samples alpha 1 from a texture whose GL format has none, and the RGBA texel keeps what a copy in brought.
    // A render target keeps alpha 1 in memory and cannot be swizzled; any other texture reads alpha 1 through its
    // swizzle, which sampling and reads apply (MTLTexture.h:249-252); its readback forces 1.
    const bool swizzleAlphaOne = !hasAlphaInGLFormat(desc.glFormat) && !(desc.usage & TEXTURE_RENDER_TARGET);
    const auto channel = [swizzleAlphaOne](Swizzle swizzle)
    { return metalSwizzle(swizzleAlphaOne && swizzle == Swizzle::A ? Swizzle::One : swizzle); };
    descriptor.swizzle = MTLTextureSwizzleChannelsMake(channel(desc.swizzle.r), channel(desc.swizzle.g),
                                                       channel(desc.swizzle.b), channel(desc.swizzle.a));
    const MetalTextureStorage storage = metalTextureStorage(desc);
    descriptor.storageMode = storage.mode;
    descriptor.usage = storage.usage;
    IdMTLTexture texture = [device.mtlDevice() newTextureWithDescriptor:descriptor];
    if (!texture)
    {
        warnCreateFailed("texture", debugName, "Metal gave no texture");
        return nullptr;
    }
    if (device.debugLabels() && !debugName.empty())
    {
        texture.label = metalString(debugName);
    }
    // Resident from creation; the set is committed before the next submit. A memoryless texture has no memory to
    // make resident.
    if (texture.storageMode != MTLStorageModeMemoryless)
    {
        device.residency().admit(texture, debugName);
    }
    Ref<MetalTexture> result = new MetalTexture(device, desc, texture);
    result->setDebugName(debugName);
    return result;
}

Ref<Texture> createMetalDefaultTexture(MetalDevice& device, TextureType type)
{
    TextureDesc desc;
    desc.type = type;
    desc.format = Format::RGBA8Unorm;
    const char* name = type == TextureType::Tex2D  ? "default 2D texture"
                       : type == TextureType::Cube  ? "default cube texture"
                       : type == TextureType::Tex3D ? "default 3D texture"
                                                    : "default cube array texture";
    MTLTextureDescriptor* descriptor = nil;
    if (type == TextureType::Tex2D)
    {
        descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                        width:1
                                                                       height:1
                                                                    mipmapped:NO];
    }
    else if (type == TextureType::Tex3D)
    {
        // One texel deep (MTLTextureType3D), the one slice filled below.
        descriptor = [MTLTextureDescriptor new];
        descriptor.textureType = MTLTextureType3D;
        descriptor.pixelFormat = MTLPixelFormatRGBA8Unorm;
        descriptor.width = 1;
        descriptor.height = 1;
        descriptor.depth = 1;
    }
    else
    {
        descriptor = [MTLTextureDescriptor textureCubeDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                           size:1
                                                                      mipmapped:NO];
        if (type == TextureType::CubeArray)
        {
            descriptor.textureType = MTLTextureTypeCubeArray;
            descriptor.arrayLength = 1; // one cube, six slices
        }
    }
    // Shared (MTLResource.h:60-63): the CPU fills it now with replaceRegion (MTLTexture.h:508-511), no GPU
    // copy, so no pass breaks for it.
    descriptor.storageMode = MTLStorageModeShared;
    descriptor.usage = MTLTextureUsageShaderRead;
    IdMTLTexture texture = [device.mtlDevice() newTextureWithDescriptor:descriptor];
    if (!texture)
    {
        warnCreateFailed("texture", name, "Metal gave no texture");
        return nullptr;
    }
    constexpr std::array<U8, 4> BLACK = { 0, 0, 0, 255 };
    // A 3D texture has one slice; its depth is the region's (MTLRegionMake2D is one texel deep at z 0).
    const NSUInteger slices = type == TextureType::Tex2D || type == TextureType::Tex3D ? 1 : 6;
    for (NSUInteger slice = 0; slice < slices; ++slice)
    {
        [texture replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 slice:slice withBytes:BLACK.data()
                   bytesPerRow:4
                 bytesPerImage:4];
    }
    if (device.debugLabels())
    {
        texture.label = metalString(name);
    }
    device.residency().admit(texture, name);
    Ref<MetalTexture> result = new MetalTexture(device, desc, texture);
    result->setDebugName(name);
    return result;
}

Ref<Texture> createMetalDefaultShadowTexture(MetalDevice& device)
{
    TextureDesc desc;
    desc.format = Format::Depth32Float;
    desc.usage = TEXTURE_SAMPLED;
    const char* name = "default shadow texture";
    MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                                                          width:1
                                                                                         height:1
                                                                                      mipmapped:NO];
    // Private like every depth texture of the backend (metalTextureStorage); never written, as compare function
    // Never ignores the texel (createMetalDefaultShadowTexture's comment in llrhimetal-objc.h).
    descriptor.storageMode = MTLStorageModePrivate;
    descriptor.usage = MTLTextureUsageShaderRead;
    IdMTLTexture texture = [device.mtlDevice() newTextureWithDescriptor:descriptor];
    if (!texture)
    {
        warnCreateFailed("texture", name, "Metal gave no texture");
        return nullptr;
    }
    if (device.debugLabels())
    {
        texture.label = metalString(name);
    }
    device.residency().admit(texture, name);
    Ref<MetalTexture> result = new MetalTexture(device, desc, texture);
    result->setDebugName(name);
    return result;
}

namespace
{
    /// An upload's copy of its converted texels, on the main thread's compute encoder or the upload queue's
    /// blit encoder, which take the same copy (MTL4ComputeCommandEncoder.h:393, MTLBlitCommandEncoder.h:79). Never
    /// flipped. Row i of the data is the texture's row y + i, which is GL's row y + i. The layer is the slice:
    /// a cube's face in GL's order, a cube array's cube * 6 + face. A depth and stencil texture takes the depth.
    template <class Copies>
    API_AVAILABLE(macos(26.0)) void encodeUploadCopy(Copies copies, IdMTLBuffer buffer, U64 offset, U32 rowBytes,
                                                     U64 imageBytes, Texture* texture, U32 level, U32 layer,
                                                     const Region& region)
    {
        // A 3D texture's layer is its z: slice 0, origin z (MTLBlitCommandEncoder.h's copy into a volume).
        const bool volume = texture->desc().type == TextureType::Tex3D;
        [copies copyFromBuffer:buffer
                  sourceOffset:offset
             sourceBytesPerRow:rowBytes
           sourceBytesPerImage:imageBytes
                    sourceSize:MTLSizeMake(region.width, region.height, 1)
                     toTexture:metalTexture(texture)
              destinationSlice:volume ? 0 : layer
              destinationLevel:level
             destinationOrigin:MTLOriginMake(region.x, region.y, volume ? layer : 0)
                       options:hasStencil(texture->desc().format) ? MTLBlitOptionDepthFromDepthStencil
                                                                  : MTLBlitOptionNone];
    }

    /// metalZeroFill's copies from its buffer of zeros at offset, on either encoder; the parts do not overlap
    /// the region or each other, so no order between them is needed (MTL4CommandEncoder.h:55-58).
    template <class Copies>
    API_AVAILABLE(macos(26.0)) void encodeZeroCopies(Copies copies, const MetalZeroFill& fill, IdMTLBuffer buffer,
                                                     U64 offset, Texture* texture, U32 level, U32 layer)
    {
        const bool volume = texture->desc().type == TextureType::Tex3D; // as encodeUploadCopy
        for (const Region& part : fill.parts)
        {
            if (part.width == 0 || part.height == 0)
            {
                continue;
            }
            [copies copyFromBuffer:buffer
                      sourceOffset:offset
                 sourceBytesPerRow:NSUInteger(part.width) * fill.pixelBytes
               sourceBytesPerImage:NSUInteger(part.width) * part.height * fill.pixelBytes
                        sourceSize:MTLSizeMake(part.width, part.height, 1)
                         toTexture:metalTexture(texture)
                  destinationSlice:volume ? 0 : layer
                  destinationLevel:level
                 destinationOrigin:MTLOriginMake(part.x, part.y, volume ? layer : 0)
                           options:MTLBlitOptionNone];
        }
    }
}

void metalUploadTexture(MetalDevice& device, Texture* texture, U32 level, U32 layer, const Region& region,
                        const PixelLayout& layout, std::span<const std::byte> data)
{
    if (!texture)
    {
        return;
    }
    const TextureDesc& desc = texture->desc();
    // GL reads the bytes at the call, so they go, converted, to the frame's ring now: in one pass straight into
    // the ring's memory. A refusal gives its WARN line in metalUploadSize, before any ring bytes are taken.
    const MetalUploadSize size = metalUploadSize(desc, layout, data.size(), region);
    if (size.rowBytes == 0)
    {
        return;
    }
    const MetalAllocation staging = device.frameAllocation(size.bytes);
    const U32 rowBytes =
        staging.buffer ? metalConvertUploadInto(desc, layout, data, region, staging.cpu, size.bytes) : 0;
    IdMTL4ComputeCommandEncoder copies = rowBytes ? device.beginUploadCopies() : nil;
    if (!copies)
    {
        return;
    }
    encodeUploadCopy(copies, staging.buffer, staging.offset, rowBytes, size.bytes, texture, level, layer, region);
    metalZeroAroundFirstWrite(device, copies, texture, level, layer, region); // GL's zeros around it
    // The encoder ends at once, so copies after the upload get an encoder of their own.
    device.endCopies();
    metalMarkWritten(texture, level, layer); // the level and layer is no longer unwritten
}

std::optional<MetalZeroFill> metalZeroFill(Texture* texture, U32 level, U32 layer, const Region& region)
{
    if (!texture || metalWritten(texture, level, layer))
    {
        return std::nullopt;
    }
    const TextureDesc& desc = texture->desc();
    const std::optional<PixelLayout> texel = metalReadbackLayout(desc.format);
    if (!texel || texel->components == PixelLayout::Components::Depth || blockBytes(desc.format) != 0
        || desc.samples > 1)
    {
        return std::nullopt;
    }
    const U32 width = std::max(1u, desc.width >> level);
    const U32 height = std::max(1u, desc.height >> level);
    const U32 left = std::min(region.x, width);
    const U32 top = std::min(region.y, height);
    const U32 right = std::min(region.x + region.width, width);
    const U32 bottom = std::min(region.y + region.height, height);
    MetalZeroFill fill;
    // The rows before and after the region, whole, and the region's rows left and right of it: none overlaps it.
    fill.parts = { { { 0, 0, width, top },
                     { 0, bottom, width, height - bottom },
                     { 0, top, left, bottom - top },
                     { right, top, width - right, bottom - top } } };
    for (const Region& part : fill.parts)
    {
        fill.largest = std::max(fill.largest, U64(part.width) * part.height);
    }
    if (fill.largest == 0)
    {
        return std::nullopt; // the write covers the level and slice
    }
    const U32 components = componentCount(texel->components);
    fill.componentSize = componentBytes(texel->type);
    fill.pixelBytes = components * fill.componentSize;
    // GL reads alpha 1 where its format keeps none, as an upload writes it there
    fill.alphaOne = components == 4 && !hasAlphaInGLFormat(desc.glFormat);
    fill.one = fill.alphaOne ? alphaOne(texel->type) : 0;
    return fill;
}

void metalFillZeros(const MetalZeroFill& fill, std::byte* cpu)
{
    memset(cpu, 0, size_t(fill.bytes()));
    if (fill.alphaOne)
    {
        for (U64 pixel = 0; pixel < fill.largest; ++pixel)
        {
            memcpy(cpu + pixel * fill.pixelBytes + 3 * fill.componentSize, &fill.one, fill.componentSize);
        }
    }
}

void metalZeroAroundFirstWrite(MetalDevice& device, IdMTL4ComputeCommandEncoder copies, Texture* texture, U32 level,
                               U32 layer, const Region& region)
{
    if (!copies)
    {
        return;
    }
    const std::optional<MetalZeroFill> fill = metalZeroFill(texture, level, layer, region);
    if (!fill)
    {
        return;
    }
    const MetalAllocation zeros = device.frameAllocation(fill->bytes());
    if (!zeros.buffer)
    {
        return;
    }
    metalFillZeros(*fill, zeros.cpu);
    encodeZeroCopies(copies, *fill, zeros.buffer, zeros.offset, texture, level, layer);
}

// The upload context ---------------------------------------------------------------------------------

MetalUploadFence::MetalUploadFence(IdMTLSharedEvent event, U64 value, std::vector<Ref<Texture>> mips,
                                   std::shared_ptr<MetalUploadState> state)
:   mEvent(event), mValue(value), mMips(std::move(mips)), mState(std::move(state))
{
}

bool MetalUploadFence::signaled() const
{
    return mValue == 0 || mEvent.signaledValue >= mValue;
}

void MetalUploadFence::wait()
{
    if (std::this_thread::get_id() == mState->recordingThread)
    {
        ++mState->mainThreadWaits; // the swap waits on the GPU instead (MetalEncoder::waitForFence)
    }
    if (!signaled() && ![mEvent waitUntilSignaledValue:mValue timeoutMS:METAL_UPLOAD_WAIT_MS])
    {
        LL_WARNS("RHI") << "An upload fence did not signal within " << METAL_UPLOAD_WAIT_MS << " ms" << LL_ENDL;
    }
}

MetalUploadContext::MetalUploadContext(MetalDevice& device)
:   mDevice(device), mRecordingThread(std::this_thread::get_id()), mState(std::make_shared<MetalUploadState>())
{
    mState->recordingThread = mRecordingThread;
}

MetalUploadContext::Caller MetalUploadContext::caller(const char* call)
{
    const std::thread::id self = std::this_thread::get_id();
    if (self == mRecordingThread)
    {
        return Caller::Recording;
    }
    std::thread::id upload; // none: the first other thread to call becomes the upload thread
    if (mUploadThread.compare_exchange_strong(upload, self) || upload == self)
    {
        return Caller::Upload;
    }
    LL_WARNS("RHI") << call << " on a second upload thread is refused: one upload thread (decision 3)" << LL_ENDL;
    return Caller::Other;
}

IdMTLCommandBuffer MetalUploadContext::commands()
{
    if (mCommands)
    {
        return mCommands;
    }
    if (!mQueue)
    {
        // Made at the first upload, so a viewer without the upload thread pays nothing (the design's choice 1).
        mQueue = [mDevice.mtlDevice() newCommandQueue];
        mEvent = [mDevice.mtlDevice() newSharedEvent];
        if (!mQueue || !mEvent)
        {
            LL_WARNS("RHI") << "Metal gave no upload queue or event" << LL_ENDL;
            mQueue = nil;
            mEvent = nil;
            return nil;
        }
        mQueue.label = @"Uploads";
    }
    // The command buffer keeps what its encoders use, the staging buffers and the textures, until it completes
    // (MTLCommandQueue.h's commandBuffer); its blit encoders track hazards between them.
    mCommands = [mQueue commandBuffer];
    if (!mCommands)
    {
        LL_WARNS("RHI") << "Metal gave no upload command buffer" << LL_ENDL;
        return nil;
    }
    mCommands.label = @"Uploads";
    std::lock_guard<std::mutex> lock(mGateMutex);
    if (mGate)
    {
        [mCommands encodeWaitForEvent:mGate value:mGateValue];
        mGate = nil;
    }
    return mCommands;
}

void MetalUploadContext::gateNextCommands(IdMTLSharedEvent gate, U64 value)
{
    std::lock_guard<std::mutex> lock(mGateMutex);
    mGate = gate;
    mGateValue = value;
}

void MetalUploadContext::updateBuffer(Buffer* buffer, U64 offset, std::span<const std::byte> data)
{
    if (caller("updateBuffer") == Caller::Recording)
    {
        mDevice.encoder().updateBuffer(buffer, offset, data);
    }
    else if (mUploadThread.load() == std::this_thread::get_id())
    {
        // The design's choice 1: no caller (LLImageGL uploads only textures through the upload context).
        LL_WARNS("RHI") << "updateBuffer on the upload thread is refused: buffers are updated on the main thread"
                        << LL_ENDL;
    }
}

void MetalUploadContext::uploadTexture(Texture* texture, U32 level, U32 layer, const Region& region,
                                       const PixelLayout& layout, std::span<const std::byte> data)
{
    const Caller from = caller("uploadTexture");
    if (from == Caller::Recording)
    {
        mDevice.encoder().uploadTexture(texture, level, layer, region, layout, data);
        return;
    }
    if (from != Caller::Upload)
    {
        return;
    }
    @autoreleasepool // the upload thread has no run loop to drain Metal's autoreleased objects
    {
        const std::string why = texture ? validateUpload(texture->desc(), level, layer, region, layout, data.size())
                                        : "no texture";
        if (!why.empty())
        {
            LL_WARNS("RHI") << "uploadTexture: " << why << LL_ENDL;
            return;
        }
        const TextureDesc& desc = texture->desc();
        if (desc.samples > 1)
        {
            metalNotPorted("uploadTexture into a multisampled texture", "P3-08"); // as the encoder
            return;
        }
        // The encoder's bytes and zeros in one Shared staging buffer of the upload's own, as the ring
        // is the main thread's; the bytes converted straight into it.
        const MetalUploadSize size = metalUploadSize(desc, layout, data.size(), region);
        if (size.rowBytes == 0)
        {
            return;
        }
        const std::optional<MetalZeroFill> fill = metalZeroFill(texture, level, layer, region);
        const U64 zeroOffset = (size.bytes + METAL_RING_ALIGNMENT - 1) / METAL_RING_ALIGNMENT * METAL_RING_ALIGNMENT;
        const U64 length = fill ? zeroOffset + fill->bytes() : size.bytes;
        IdMTLBuffer staging = [mDevice.mtlDevice() newBufferWithLength:length options:MTLResourceStorageModeShared];
        IdMTLCommandBuffer buffer = staging ? commands() : nil;
        IdMTLBlitCommandEncoder copies = buffer ? [buffer blitCommandEncoder] : nil;
        if (!copies)
        {
            LL_WARNS("RHI") << "Metal gave no staging buffer or blit encoder for an upload" << LL_ENDL;
            return;
        }
        std::byte* cpu = static_cast<std::byte*>(staging.contents);
        const U32 rowBytes = metalConvertUploadInto(desc, layout, data, region, cpu, size.bytes);
        if (rowBytes == 0)
        {
            [copies endEncoding]; // nothing encoded: refused after the staging was made (never: metalUploadSize agreed)
            return;
        }
        encodeUploadCopy(copies, staging, 0, rowBytes, size.bytes, texture, level, layer, region);
        if (fill)
        {
            metalFillZeros(*fill, cpu + zeroOffset);
            encodeZeroCopies(copies, *fill, staging, zeroOffset, texture, level, layer);
        }
        // One encoder per upload, as the main thread's, so a later upload of the same level runs after it.
        [copies endEncoding];
        metalMarkWritten(texture, level, layer); // the main thread sees it after the swap
        ++mState->uploads;
    }
}

void MetalUploadContext::generateMipmaps(Texture* texture)
{
    const Caller from = caller("generateMipmaps");
    if (from == Caller::Recording)
    {
        mDevice.encoder().generateMipmaps(texture);
        return;
    }
    if (from != Caller::Upload)
    {
        return;
    }
    const std::string why = texture ? validateMipmaps(texture->desc()) : "no texture";
    if (!why.empty())
    {
        LL_WARNS("RHI") << "generateMipmaps: " << why << LL_ENDL;
        return;
    }
    // The main thread's encoder makes them at waitForFence, after the queue's wait, with the same filter as without
    // the thread, as programs and pipelines are the main thread's.
    mMips.emplace_back(texture);
    ++mState->deferredMips;
}

Ref<Fence> MetalUploadContext::submit()
{
    const Caller from = caller("submit");
    if (from != Caller::Upload)
    {
        // The recording thread's uploads are on the main encoder's command buffer: nothing to wait for.
        return new MetalUploadFence(nil, 0, {}, mState);
    }
    if (!mCommands)
    {
        // Nothing recorded: 0, signalled at once; mips asked for after an earlier submit wait for its value.
        return new MetalUploadFence(mEvent, mMips.empty() ? 0 : mValue, std::exchange(mMips, {}), mState);
    }
    @autoreleasepool
    {
        IdMTLCommandBuffer buffer = std::exchange(mCommands, nil);
        const U64 value = ++mValue;
        [buffer encodeSignalEvent:mEvent value:value];
        // The handler holds the counters and the event, never the device.
        std::shared_ptr<MetalUploadState> state = mState;
        IdMTLSharedEvent event = mEvent;
        [buffer addCompletedHandler:^(IdMTLCommandBuffer done) {
            if (done.error)
            {
                // Held for the main thread (MetalFailureLog)
                const std::string line =
                    std::string("GPU error in an upload command buffer: ") + done.error.description.UTF8String;
                if (state->failures)
                {
                    state->failures->add(MetalFailureLog::Kind::Upload, line);
                }
                ++state->errors;
                // The signal after the failed work may never come: set from the CPU, so no queue waits forever.
                if (event.signaledValue < value)
                {
                    event.signaledValue = value;
                }
            }
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                --state->outstanding;
            }
            state->changed.notify_all();
        }];
        {
            std::lock_guard<std::mutex> lock(mState->mutex);
            ++mState->outstanding;
        }
        [buffer commit];
        ++mState->submits;
        return new MetalUploadFence(mEvent, value, std::exchange(mMips, {}), mState);
    }
}

void MetalUploadContext::drain()
{
    std::unique_lock<std::mutex> lock(mState->mutex);
    if (!mState->changed.wait_for(lock, std::chrono::milliseconds(METAL_UPLOAD_WAIT_MS),
                                  [this]() { return mState->outstanding == 0; }))
    {
        LL_WARNS("RHI") << mState->outstanding << " upload command buffers did not complete within "
                        << METAL_UPLOAD_WAIT_MS << " ms" << LL_ENDL;
    }
}

void MetalUploadContext::dropPending()
{
    if (!mMips.empty())
    {
        LL_INFOS("RHI") << mMips.size() << " textures noted for mipmaps were never submitted; let go with the device"
                        << LL_ENDL;
    }
    mMips.clear();
    mCommands = nil; // never committed, so the queue never ran it
}

API_AVAILABLE(macos(26.0)) MetalUploadCounts metalUploadCounts(Device& device)
{
    MetalUploadCounts counts;
    if (device.backend() != BackendType::Metal)
    {
        return counts;
    }
    const MetalUploadState& state = static_cast<MetalDevice&>(device).metalUploads().state();
    counts.submits = state.submits.load();
    counts.uploads = state.uploads.load();
    counts.deferredMips = state.deferredMips.load();
    counts.signalledAtHandover = state.signalledAtHandover.load();
    counts.queueWaits = state.queueWaits.load();
    counts.mainThreadWaits = state.mainThreadWaits.load();
    counts.errors = state.errors.load();
    return counts;
}

} // namespace LL::RHI

#endif // LL_RHI_METAL
