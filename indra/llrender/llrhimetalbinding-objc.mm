/**
 * @file llrhimetalbinding-objc.mm
 * @brief The Metal backend's one binding and residency site
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

// The one binding and residency site: MetalResidency is the only code that names the residency
// set's selectors or MTLResidencySet, and bindForDraw the only code that writes argument tables;
// scripts/metal/residency-site.py checks both on the tree.

namespace LL::RHI
{

namespace
{
    constexpr U32 TABLE_BUFFERS = METAL_TABLE_BUFFERS;
    constexpr U32 TABLE_TEXTURES = METAL_TABLE_TEXTURES;
    constexpr U32 TABLE_SAMPLERS = METAL_TABLE_SAMPLERS;
    /// LLVertexBuffer's TYPE_VERTEX and TYPE_TEXTURE_INDEX: the texture index is the position's
    /// fourth component, 12 bytes into the vertex stream.
    constexpr U32 TYPE_VERTEX = 0;
    constexpr U32 TYPE_TEXTURE_INDEX = 13;
}

// The residency set -------------------------------------------------------------------------------

bool MetalResidency::attach(IdMTL4CommandQueue queue)
{
    MTLResidencySetDescriptor* descriptor = [MTLResidencySetDescriptor new];
    descriptor.label = @"Viewer Lab";
    NSError* error = nil;
    IdMTLResidencySet set = [queue.device newResidencySetWithDescriptor:descriptor error:&error];
    if (!set)
    {
        warnCreateFailed("device", "Metal", error ? error.description.UTF8String : "no residency set");
        return false;
    }
    [queue addResidencySet:set];
    std::lock_guard<std::mutex> lock(mMutex);
    mSet = set;
    return true;
}

void MetalResidency::admit(IdMTLAllocation allocation, std::string_view name)
{
    if (!allocation)
    {
        LL_WARNS("RHI") << "Residency: '" << name << "' has no allocation to admit" << LL_ENDL;
        return;
    }
    CensusEntry entry;
    entry.bytes = allocation.allocatedSize;
    if ([allocation conformsToProtocol:@protocol(MTLTexture)])
    {
        IdMTLTexture texture = (IdMTLTexture)allocation;
        entry.group = std::to_string(texture.width) + "x" + std::to_string(texture.height);
        if (texture.depth > 1 || texture.arrayLength > 1)
        {
            entry.group += "x" + std::to_string(std::max(texture.depth, texture.arrayLength));
        }
        entry.group += " fmt" + std::to_string(NSUInteger(texture.pixelFormat)) + " L" + std::to_string(texture.mipmapLevelCount);
        if (texture.sampleCount > 1)
        {
            entry.group += " s" + std::to_string(texture.sampleCount);
        }
        if (texture.textureType == MTLTextureTypeCube || texture.textureType == MTLTextureTypeCubeArray)
        {
            entry.group += " cube";
        }
        if (!name.empty())
        {
            entry.group += " '" + std::string(name) + "'";
        }
    }
    else
    {
        entry.group = "buffer";
        if (!name.empty())
        {
            entry.group += " " + std::string(name);
        }
    }
    std::lock_guard<std::mutex> lock(mMutex);
    [mSet addAllocation:allocation];
    mAdmittedSince[entry.group] += entry.bytes;
    mCensus[(__bridge const void*)allocation] = std::move(entry);
    mChanged = true;
}

void MetalResidency::retire(IdMTLAllocation allocation)
{
    std::lock_guard<std::mutex> lock(mMutex);
    if (auto found = mCensus.find((__bridge const void*)allocation); found != mCensus.end())
    {
        mRetiredSince[found->second.group] += found->second.bytes;
        mCensus.erase(found);
    }
    // A test's dropForTest may have taken it out already.
    if ([mSet containsAllocation:allocation])
    {
        [mSet removeAllocation:allocation];
        mChanged = true;
    }
}

std::string MetalResidency::census(size_t top, U64& totalBytes) const
{
    struct Group
    {
        U64 count = 0;
        U64 bytes = 0;
    };
    std::unordered_map<std::string, Group> groups;
    std::unordered_map<std::string, U64> admitted;
    std::unordered_map<std::string, U64> retired;
    totalBytes = 0;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        admitted.swap(mAdmittedSince);
        retired.swap(mRetiredSince);
        for (const auto& [allocation, entry] : mCensus)
        {
            Group& group = groups[entry.group];
            ++group.count;
            group.bytes += entry.bytes;
            totalBytes += entry.bytes;
        }
    }
    std::vector<std::pair<std::string, Group>> sorted(groups.begin(), groups.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second.bytes > b.second.bytes; });
    // What came and went since the last census, its five largest groups each
    const auto churn = [](const std::unordered_map<std::string, U64>& bytes)
    {
        std::vector<std::pair<std::string, U64>> list(bytes.begin(), bytes.end());
        std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        U64 total = 0;
        for (const auto& item : list)
        {
            total += item.second;
        }
        std::string text = std::to_string(total / (1024 * 1024)) + " MB";
        for (size_t i = 0; i < list.size() && i < 5; ++i)
        {
            text += (i ? ", " : " (") + list[i].first + " " + std::to_string(list[i].second / (1024 * 1024)) + " MB";
        }
        return list.empty() ? text : text + ")";
    };
    std::string text = "since the last: admitted " + churn(admitted) + ", retired " + churn(retired) + " | ";
    for (size_t i = 0; i < sorted.size() && i < top; ++i)
    {
        text += (i ? "; " : "") + sorted[i].first + " x" + std::to_string(sorted[i].second.count) + " "
                + std::to_string(sorted[i].second.bytes / (1024 * 1024)) + " MB";
    }
    return text;
}

bool MetalResidency::contains(IdMTLAllocation allocation) const
{
    std::lock_guard<std::mutex> lock(mMutex);
    return [mSet containsAllocation:allocation];
}

bool MetalResidency::containsTexture(IdMTLTexture texture) const
{
    // A view has no allocation of its own: the texture it views is in the set (MTLTexture.h's parentTexture). A
    // texture placed in a heap is resident through its heap, which is the set's allocation (MTLHeap is an
    // MTLAllocation, MTLHeap.h).
    IdMTLTexture root = texture;
    while (root.parentTexture)
    {
        root = root.parentTexture;
    }
    if (root.heap)
    {
        return contains(root.heap);
    }
    return contains(root);
}

void MetalResidency::commitIfChanged()
{
    // A texture the upload thread makes meanwhile is either in this commit or flags the next.
    std::lock_guard<std::mutex> lock(mMutex);
    if (mChanged)
    {
        [mSet commit];
        mChanged = false;
    }
}

U64 MetalResidency::count() const
{
    std::lock_guard<std::mutex> lock(mMutex);
    return mSet.allocationCount;
}

void MetalResidency::dropForTest(IdMTLAllocation allocation)
{
    std::lock_guard<std::mutex> lock(mMutex);
    [mSet removeAllocation:allocation];
    mChanged = true;
}

bool MetalResidency::attachLayer(IdMTL4CommandQueue queue, CAMetalLayer* layer)
{
    // The layer keeps its set up to date itself and it must not be changed (CAMetalLayer.h:160-171); none
    // without a device or on a device without residency sets.
    IdMTLResidencySet set = layer.residencySet;
    if (!set)
    {
        warnCreateFailed("device", "Metal", "the CAMetalLayer has no residency set");
        return false;
    }
    [queue addResidencySet:set];
    std::lock_guard<std::mutex> lock(mMutex);
    mLayerSet = set;
    return true;
}

void MetalResidency::detachLayer(IdMTL4CommandQueue queue)
{
    std::lock_guard<std::mutex> lock(mMutex);
    if (mLayerSet)
    {
        [queue removeResidencySet:mLayerSet];
        mLayerSet = nil;
    }
}

IdMTLResidencySet MetalResidency::layerSet() const
{
    std::lock_guard<std::mutex> lock(mMutex);
    return mLayerSet;
}

// The checks --------------------------------------------------------------------------------------

std::string metalCheckIndexRange(const Buffer& indices, IndexType type, U32 first, U32 count, U32 vertexCount,
                                 std::span<const std::byte> cpuIndices)
{
    if (count == 0)
    {
        return {};
    }
    const U64 size = type == IndexType::U16 ? 2 : 4;
    const U64 end = U64(first) + count;
    const std::string buffer = "index buffer '" + indices.debugName() + "'";
    if (end * size > indices.desc().size)
    {
        return "indices " + std::to_string(first) + "-" + std::to_string(end - 1) + " are past the end of " + buffer +
               " (" + std::to_string(indices.desc().size / size) + " indices)";
    }
    for (U64 i = first; i < end && (i + 1) * size <= cpuIndices.size(); ++i)
    {
        U32 value = 0;
        if (type == IndexType::U16)
        {
            U16 small = 0;
            std::memcpy(&small, cpuIndices.data() + i * size, sizeof(small));
            value = small;
        }
        else
        {
            std::memcpy(&value, cpuIndices.data() + i * size, sizeof(value));
        }
        if (value >= vertexCount)
        {
            return "index " + std::to_string(i) + " of " + buffer + " is " + std::to_string(value) + ", past the " +
                   std::to_string(vertexCount) + " vertices";
        }
    }
    return {};
}

U64 metalWholeVertices(Buffer* vertices, const VertexStreams& streams, U32 attributes)
{
    U64 whole = U64(U32(-1));
    for (U32 type = 0; type < VertexStreams::MAX_ATTRIBUTES; ++type)
    {
        if (attributes & (1u << type))
        {
            if (!vertices)
            {
                return 0;
            }
            const U64 offset = streams.offset[type == TYPE_TEXTURE_INDEX ? TYPE_VERTEX : type];
            const U64 length = vertices->desc().size;
            whole = std::min<U64>(whole, length > offset ? (length - offset) / metalVertexStride(type) : 0);
        }
    }
    return whole;
}

void MetalEncoder::writeAddress(size_t stage, U32 index, MTLGPUAddress address)
{
    MetalTableShadow& shadow = mShadows[stage];
    if (!shadow.addressWritten[index] || shadow.addresses[index] != address)
    {
        [mTables[stage] setAddress:address atIndex:index];
        shadow.addresses[index] = address;
        shadow.addressWritten[index] = true;
        ++mTableWrites;
    }
}

void MetalEncoder::writeTexture(size_t stage, U32 index, MTLResourceID texture)
{
    MetalTableShadow& shadow = mShadows[stage];
    if (!shadow.textureWritten[index] || shadow.textures[index]._impl != texture._impl)
    {
        [mTables[stage] setTexture:texture atIndex:index];
        shadow.textures[index] = texture;
        shadow.textureWritten[index] = true;
        ++mTableWrites;
    }
}

void MetalEncoder::writeSampler(size_t stage, U32 index, MTLResourceID sampler)
{
    MetalTableShadow& shadow = mShadows[stage];
    if (!shadow.samplerWritten[index] || shadow.samplers[index]._impl != sampler._impl)
    {
        [mTables[stage] setSamplerState:sampler atIndex:index];
        shadow.samplers[index] = sampler;
        shadow.samplerWritten[index] = true;
        ++mTableWrites;
    }
}

void MetalEncoder::forgetTexture(MTLResourceID texture)
{
    for (MetalTableShadow& shadow : mShadows)
    {
        for (U32 index = 0; index < METAL_TABLE_TEXTURES; ++index)
        {
            if (shadow.textureWritten[index] && shadow.textures[index]._impl == texture._impl)
            {
                shadow.textureWritten[index] = false;
            }
        }
    }
}

bool MetalEncoder::shadowHolds(MTLResourceID texture) const
{
    for (const MetalTableShadow& shadow : mShadows)
    {
        for (U32 index = 0; index < METAL_TABLE_TEXTURES; ++index)
        {
            if (shadow.textureWritten[index] && shadow.textures[index]._impl == texture._impl)
            {
                return true;
            }
        }
    }
    return false;
}

U32 MetalEncoder::shadowTextures() const
{
    U32 count = 0;
    for (const MetalTableShadow& shadow : mShadows)
    {
        count += U32(std::count(shadow.textureWritten.begin(), shadow.textureWritten.end(), true));
    }
    return count;
}

bool MetalEncoder::makeTables(const char* call)
{
    if (!mTables[0])
    {
        // MTL4ArgumentTable.h:24-42; MTLDevice.h:1425-1433. Without an NSError: Metal API Validation
        // can hand back a released one from a synchronous Metal 4 call.
        MTL4ArgumentTableDescriptor* descriptor = [MTL4ArgumentTableDescriptor new];
        descriptor.maxBufferBindCount = TABLE_BUFFERS;
        descriptor.maxTextureBindCount = TABLE_TEXTURES;
        descriptor.maxSamplerStateBindCount = TABLE_SAMPLERS;
        descriptor.initializeBindings = YES;
        for (size_t s = 0; s < mTables.size(); ++s)
        {
            mTables[s] = [mDevice.mtlDevice() newArgumentTableWithDescriptor:descriptor error:nil];
        }
        if (!mTables[0] || !mTables[1] || !mTables[2])
        {
            mTables = {};
            LL_WARNS("RHI") << call << ": Metal gave no argument table" << LL_ENDL;
            return false;
        }
        // New tables hold no bindings (initializeBindings), so every place is written again.
        mShadows = {};
        if (mNative)
        {
            [mNative setArgumentTable:mTables[0] atStages:MTLRenderStageVertex];
            [mNative setArgumentTable:mTables[1] atStages:MTLRenderStageFragment];
        }
        if (mCompute)
        {
            [mCompute setArgumentTable:mTables[2]]; // MTL4ComputeCommandEncoder.h:661
        }
    }
    return true;
}

bool MetalEncoder::bindForClear(Program& program, const std::array<F32, 4>& rgba, F32 depthValue)
{
    if (!makeTables("clear"))
    {
        return false;
    }
    // The clear program's default blocks with clear_color and clear_depth (metalClearProgramDesc), in
    // the frame's ring; the tracker's bindings are not touched.
    const ProgramReflection& reflection = program.reflection();
    const MetalProgramLayout& layout = *metalProgramLayout(&program);
    const MetalStageLayout* stages[2] = { &layout.vertex, &layout.fragment };
    for (size_t s = 0; s < 2; ++s)
    {
        if (stages[s]->blockSize == 0)
        {
            continue;
        }
        const MetalAllocation uniforms = mDevice.frameAllocation(stages[s]->blockSize);
        if (!uniforms.buffer)
        {
            return false; // the ring wrote the WARN line
        }
        if (mDevice.debugChecks() && !mDevice.residency().contains(uniforms.buffer))
        {
            LL_WARNS("RHI") << "clear: uniform ring is not resident (program '" << program.debugName() << "')" << LL_ENDL;
            return false;
        }
        std::memset(uniforms.cpu, 0, stages[s]->blockSize);
        for (size_t i = 0; i < reflection.uniforms.size() && i < stages[s]->places.size(); ++i)
        {
            const MetalUniformPlace& place = stages[s]->places[i];
            const std::string& name = reflection.uniforms[i].name;
            if (name == "clear_color" && place.size >= sizeof(rgba))
            {
                std::memcpy(uniforms.cpu + place.offset, rgba.data(), sizeof(rgba));
            }
            else if (name == "clear_depth" && place.size >= sizeof(depthValue))
            {
                std::memcpy(uniforms.cpu + place.offset, &depthValue, sizeof(depthValue));
            }
        }
        writeAddress(s, 0, uniforms.buffer.gpuAddress + uniforms.offset);
        // The next draw writes its own block's address here again, whatever the shadow held.
        mShadows[s].addressWritten[0] = false;
    }
    return true;
}

bool MetalEncoder::bindForMip(Program& program, Texture* scratch, IdMTLTexture source, IdMTL4RenderCommandEncoder encoder)
{
    if (!makeTables("generateMipmaps"))
    {
        return false;
    }
    // metalMipProgramDesc: one texture, mip_source, on the fragment stage, and no default block.
    const MetalProgramLayout& layout = *metalProgramLayout(&program);
    if (layout.fragment.textureUniforms.size() != 1 || !layout.vertex.textureUniforms.empty())
    {
        LL_WARNS("RHI") << "generateMipmaps: program '" << program.debugName() << "' does not sample one texture"
                        << LL_ENDL;
        return false;
    }
    if (mDevice.debugChecks() && !mDevice.residency().contains(metalTexture(scratch)))
    {
        LL_WARNS("RHI") << "generateMipmaps: texture '" << scratch->debugName() << "' is not resident" << LL_ENDL;
        return false;
    }
    SamplerDesc desc;
    desc.filter = Filter::Bilinear;
    desc.mipmapped = false;
    desc.addressU = desc.addressV = desc.addressW = AddressMode::ClampToEdge;
    Ref<Sampler> sampler = mDevice.getSampler(desc);
    if (sampler.isNull())
    {
        return false; // getSampler wrote the WARN line
    }
    // The shadow holds the view until its release forgets it (generateMipmaps), so the next draw writes its own
    // texture here again, even one that Metal gives the view's MTLResourceID once the view is freed.
    writeTexture(1, 0, source.gpuResourceID);
    writeSampler(1, 0, metalSampler(sampler.get()).gpuResourceID);
    [encoder setArgumentTable:mTables[0] atStages:MTLRenderStageVertex];
    [encoder setArgumentTable:mTables[1] atStages:MTLRenderStageFragment];
    return true;
}

bool MetalEncoder::bindForPresent(Program& program, Texture* back, IdMTL4RenderCommandEncoder encoder)
{
    if (!makeTables("present"))
    {
        return false;
    }
    // metalPresentProgramDesc: one texture, present_source, on the fragment stage, and no default block.
    const MetalProgramLayout& layout = *metalProgramLayout(&program);
    if (layout.fragment.textureUniforms.size() != 1 || !layout.vertex.textureUniforms.empty())
    {
        LL_WARNS("RHI") << "present: program '" << program.debugName() << "' does not sample one texture" << LL_ENDL;
        return false;
    }
    // The back buffer is checked; the drawable is the encoder's attachment, bound through no table and
    // resident through the layer's set on the queue (CAMetalLayer.h:160-171), so it is not.
    if (!back || (mDevice.debugChecks() && !mDevice.residency().contains(metalTexture(back))))
    {
        LL_WARNS("RHI") << "present: the back buffer " << (back ? "is not resident" : "is missing") << LL_ENDL;
        return false;
    }
    // texelFetch reads no sampler, but the translated stage may still name one in its table.
    SamplerDesc desc;
    desc.filter = Filter::Point;
    desc.mipmapped = false;
    desc.addressU = desc.addressV = desc.addressW = AddressMode::ClampToEdge;
    Ref<Sampler> sampler = mDevice.getSampler(desc);
    if (sampler.isNull())
    {
        return false; // getSampler wrote the WARN line
    }
    writeTexture(1, 0, metalTexture(back).gpuResourceID);
    writeSampler(1, 0, metalSampler(sampler.get()).gpuResourceID);
    [encoder setArgumentTable:mTables[0] atStages:MTLRenderStageVertex];
    [encoder setArgumentTable:mTables[1] atStages:MTLRenderStageFragment];
    return true;
}

bool MetalEncoder::bindForFormatCopy(Program& program, Texture* source, IdMTL4RenderCommandEncoder encoder)
{
    if (!makeTables("copyTexture"))
    {
        return false;
    }
    // metalFormatCopyProgramDesc: one texture, copy_source, on the fragment stage, and no default block.
    const MetalProgramLayout& layout = *metalProgramLayout(&program);
    if (layout.fragment.textureUniforms.size() != 1 || !layout.vertex.textureUniforms.empty())
    {
        LL_WARNS("RHI") << "copyTexture: program '" << program.debugName() << "' does not sample one texture" << LL_ENDL;
        return false;
    }
    if (mDevice.debugChecks() && !mDevice.residency().contains(metalTexture(source)))
    {
        LL_WARNS("RHI") << "copyTexture: texture '" << source->debugName() << "' is not resident" << LL_ENDL;
        return false;
    }
    // texelFetch reads no sampler, but the translated stage may still name one in its table (bindForPresent).
    SamplerDesc desc;
    desc.filter = Filter::Point;
    desc.mipmapped = false;
    desc.addressU = desc.addressV = desc.addressW = AddressMode::ClampToEdge;
    Ref<Sampler> sampler = mDevice.getSampler(desc);
    if (sampler.isNull())
    {
        return false; // getSampler wrote the WARN line
    }
    // The scratch's release forgets its MTLResourceID in the shadows (~MetalTexture), so a later texture that Metal
    // gives the same ID is written here again.
    writeTexture(1, 0, metalTexture(source).gpuResourceID);
    writeSampler(1, 0, metalSampler(sampler.get()).gpuResourceID);
    [encoder setArgumentTable:mTables[0] atStages:MTLRenderStageVertex];
    [encoder setArgumentTable:mTables[1] atStages:MTLRenderStageFragment];
    return true;
}

bool MetalEncoder::bindForDraw(const char* call, U32 first, U32 count, bool indexed)
{
    const auto refuse = [call](const std::string& why)
    {
        LL_WARNS("RHI") << call << ": " << why << LL_ENDL;
        return false;
    };
    Pipeline* pipeline = mTracker.pipeline();
    if (pipeline && pipeline->native())
    {
        // A native pipeline has no program layout, channels or vertex streams: its bindings are the native slots.
        return bindNativeForDraw(call);
    }
    const MetalProgramLayout* layout = metalPipelineLayout(pipeline);
    if (!layout)
    {
        return refuse("no pipeline is set");
    }
    const std::string program = "program '" + pipeline->debugName() + "'";
    // With the Metal debug setting, before the first table write, every allocation the draw
    // binds is resident and every stream it reads holds its vertices; the bindings the program
    // needs are checked always, since Metal would read what is not there.
    const bool checks = mDevice.debugChecks();
    MetalResidency& residency = mDevice.residency();
    const auto missing = [&](IdMTLAllocation allocation) { return checks && !residency.contains(allocation); };

    // The streams the pipeline reads; the tracker skips this with checks off.
    const U32 attributes = metalPipelineAttributes(pipeline);
    Buffer* vertices = mTracker.vertexBuffer();
    const VertexStreams& streams = mTracker.streams();
    if (attributes != 0 && !vertices)
    {
        return refuse(program + " reads vertex streams, and no vertex buffer is bound");
    }
    if ((attributes & ~streams.attributeMask) != 0)
    {
        return refuse(program + " reads attributes the streams of vertex buffer '" + vertices->debugName() +
                      "' lack (mask " + std::to_string(attributes & ~streams.attributeMask) + ")");
    }
    if (attributes != 0)
    {
        const std::string buffer = "vertex buffer '" + vertices->debugName() + "'";
        if (missing(metalAllocation(vertices).buffer))
        {
            return refuse(buffer + " is not resident (" + program + ")");
        }
        // Each stream holds count * stride bytes, as LLVertexBuffer lays it out; the texture index
        // is in the position's stream. An indexed draw's indices are checked below instead.
        const U64 length = vertices->desc().size;
        const U64 end = U64(first) + count;
        for (U32 type = 0; checks && !indexed && type < VertexStreams::MAX_ATTRIBUTES; ++type)
        {
            const U32 offset = streams.offset[type == TYPE_TEXTURE_INDEX ? TYPE_VERTEX : type];
            if ((attributes & (1u << type)) && offset + end * metalVertexStride(type) > length)
            {
                return refuse(buffer + ": vertices " + std::to_string(first) + "-" + std::to_string(end - 1) +
                              " of stream " + std::to_string(type) + " are past its end (" + std::to_string(length) +
                              " bytes, " + program + ")");
            }
        }
    }
    if (indexed && checks)
    {
        // The index buffer is resident, and every index it gives the draw is below the whole
        // vertices of the fewest a read stream holds. The tracker has refused a draw without an
        // index buffer or past its end (llrhi.cpp, drawIndexed).
        Buffer* indices = mTracker.indexBuffer();
        if (missing(metalAllocation(indices).buffer))
        {
            return refuse("index buffer '" + indices->debugName() + "' is not resident (" + program + ")");
        }
        const U64 whole = metalWholeVertices(vertices, streams, attributes);
        ++mIndexChecks;
        const std::string why = metalCheckIndexRange(*indices, mTracker.indexType(), first, count, U32(whole),
                                                     metalIndexBytes(indices));
        if (!why.empty())
        {
            return refuse(why + " (" + program + ")");
        }
    }

    // Each stage's textures and samplers, from the program's channels, before anything is bound.
    struct Sampled
    {
        U32 index;
        MTLResourceID texture;
        MTLResourceID sampler;
    };
    struct BoundBlock
    {
        U32 index;
        MTLGPUAddress address;
    };
    MetalProgramState& state = programState(pipeline->programId(), *layout);
    const MetalStageLayout* stages[2] = { &layout->vertex, &layout->fragment };
    std::vector<Sampled> sampled[2];
    std::vector<BoundBlock> blocks[2];
    for (size_t s = 0; s < 2; ++s)
    {
        // Each block the stage reads, from the buffer and the range setUniformBuffer gave its slot,
        // on buffer 1 + slot. With nothing bound the draw is refused: a null address would hide
        // what GL shows as undefined reads.
        for (U32 slot = 0; slot < 32; ++slot)
        {
            if (!(stages[s]->uniformBlocks & (1u << slot)))
            {
                continue;
            }
            const std::string where = program + " reads the uniform block in slot " + std::to_string(slot);
            const auto bound = mTracker.uniformBuffers().find(slot);
            const auto range = mBlockRanges.find(slot);
            if (bound == mTracker.uniformBuffers().end() || bound->second.isNull() || range == mBlockRanges.end())
            {
                return refuse(where + ", which has no uniform buffer");
            }
            const U32 size = slot < stages[s]->blockSizes.size() ? stages[s]->blockSizes[slot] : 0;
            if (checks && range->second.second < size)
            {
                return refuse(where + ": uniform buffer '" + bound->second->debugName() + "' is bound with " +
                              std::to_string(range->second.second) + " bytes, and the block has " + std::to_string(size));
            }
            const MetalAllocation memory = metalAllocation(bound->second.get());
            blocks[s].push_back({ 1 + slot, memory.buffer.gpuAddress + memory.offset + range->second.first });
        }
        for (U32 index = 0; index < stages[s]->textureUniforms.size(); ++index)
        {
            if (index >= TABLE_SAMPLERS)
            {
                return refuse(program + ": texture " + std::to_string(index) + " is past the argument table's samplers");
            }
            const U32 uniform = stages[s]->textureUniforms[index];
            const U32 channel = uniform < state.channels.size() ? state.channels[uniform] : 0;
            const UniformType type =
                index < stages[s]->textureTypes.size() ? stages[s]->textureTypes[index] : UniformType::Sampler2D;
            const std::string where = program + " samples channel " + std::to_string(channel);
            // The sampler reads the channel's slot of its own type, as a GL sampler reads its own target, so a 2D
            // sampler never gets the cube array the channel got last, nor the reverse; the read is counted when the
            // slot's texture is not the channel's current one.
            const TextureType wanted = type == UniformType::SamplerCube        ? TextureType::Cube
                                       : type == UniformType::SamplerCubeArray ? TextureType::CubeArray
                                       : type == UniformType::Sampler3D        ? TextureType::Tex3D
                                                                               : TextureType::Tex2D;
            const auto slots = mTypeSlots.find(channel);
            const TypeSlot* slot = slots != mTypeSlots.end() ? &slots->second.types[size_t(wanted)] : nullptr;
            Texture* texture = slot ? slot->texture.get() : nullptr;
            SamplerDesc desc = texture ? slot->sampling : SamplerDesc();
            if (texture)
            {
                const auto bound = mTracker.textures().find(channel);
                if (bound == mTracker.textures().end() || bound->second.get() != texture)
                {
                    ++mStats.typeSlotReads;
                }
            }
            const bool unbound = !texture;
            if (unbound)
            {
                // A channel with nothing bound reads the default texture of the sampler's type,
                // (0, 0, 0, 1) as on GL (llrhi.h:1004-1009); a shadow sampler's gives 0.
                texture = mDevice.defaultTexture(type);
                if (!texture)
                {
                    return refuse(where +
                                  ", which has no texture, and Metal has no default texture for its sampler type");
                }
            }
            if (checks && !residency.containsTexture(metalTexture(texture)))
            {
                return refuse("texture '" + texture->debugName() + "' is not resident (" + where + ")");
            }
            if (type == UniformType::Sampler2DShadow && !isDepth(texture->desc().format))
            {
                // The translated MSL declares a shadow sampler's texture as depth2d, and Metal needs a
                // depth pixel format there (GL's result is undefined), so the draw is refused always, as a
                // binding the program needs.
                return refuse(where + " as a sampler2DShadow, and texture '" + texture->debugName() +
                              "' has no depth format");
            }
            // The sampler is chosen at the draw as the GL backend chooses it
            // (llrhigl.cpp:2244-2252): samplerForDraw, and mips only for a texture that has them.
            desc = samplerForDraw(desc, type, texture->desc().format);
            desc.mipmapped = desc.mipmapped && texture->desc().mipLevels > 1;
            if (unbound && desc.compare)
            {
                // GL 4.1 gives 0 for a shadow sampler on an incomplete texture. Never never passes
                // (Apple's MTLCompareFunction.never; the SDK header MTLSampler.h:207-210 says only that it is the
                // default), so it gives 0 for every reference whatever the never-written default holds, on the
                // default texture and on a bound Depth32Float where LessEqual passes.
                desc.compareFunc = CompareFunc::Never;
            }
            Ref<Sampler> sampler = mDevice.getSampler(desc);
            if (sampler.isNull())
            {
                return false; // getSampler wrote the WARN line
            }
            mLastSamplers[channel] = desc;
            sampled[s].push_back(
                { index, metalTexture(texture).gpuResourceID, metalSampler(sampler.get()).gpuResourceID });
        }
    }

    if (!makeTables(call))
    {
        return false;
    }

    // The uniform buffers setUniformBuffer recorded.
    for (const auto& [block, bound] : mTracker.uniformBuffers())
    {
        if (bound.notNull() && missing(metalAllocation(bound.get()).buffer))
        {
            return refuse("uniform buffer '" + bound->debugName() + "' is not resident (" + program + ")");
        }
    }

    // The draw's default blocks in the frame's ring. A stage's block is copied only when a setter
    // changed it or its copy is from another frame; the ring keeps a frame's copies for the rest of
    // that frame. Each draw still sets the address, as another program's draw may have changed
    // buffer 0.
    const U64 frame = mDevice.frameNumber();
    const MetalPointBlock* point = metalPipelinePointBlock(pipeline);
    for (size_t s = 0; s < 2; ++s)
    {
        const std::vector<std::byte>& block = state.blocks[s];
        if (s == 0 && point)
        {
            // A point draw binds the variant's block below; the program's copy in the ring is then
            // not of this frame.
            state.dirty[0] = state.dirty[0] || state.frame != frame;
            continue;
        }
        if (block.empty() || (!state.dirty[s] && state.frame == frame))
        {
            continue;
        }
        const MetalAllocation uniforms = mDevice.frameAllocation(block.size());
        if (!uniforms.buffer)
        {
            return false; // the ring wrote the WARN line
        }
        if (missing(uniforms.buffer))
        {
            return refuse("uniform ring is not resident (" + program + ")");
        }
        std::copy(block.begin(), block.end(), uniforms.cpu);
        state.address[s] = uniforms.buffer.gpuAddress + uniforms.offset;
        state.dirty[s] = false;
        ++mRingBlockCopies;
    }
    state.frame = frame;
    MTLGPUAddress pointAddress = 0;
    if (point)
    {
        // The variant's vertex block, made from the program's by name with point_size the
        // tracker's point size, at each point draw.
        metalBuildPointBlock(*point, state.blocks[0], mTracker.dynamicState().pointSize, state.pointBlock);
        const MetalAllocation uniforms = mDevice.frameAllocation(state.pointBlock.size());
        if (!uniforms.buffer)
        {
            return false; // the ring wrote the WARN line
        }
        if (missing(uniforms.buffer))
        {
            return refuse("uniform ring is not resident (" + program + ")");
        }
        std::copy(state.pointBlock.begin(), state.pointBlock.end(), uniforms.cpu);
        pointAddress = uniforms.buffer.gpuAddress + uniforms.offset;
        ++mRingBlockCopies;
    }
    for (size_t s = 0; s < 2; ++s)
    {
        if (s == 0 && point)
        {
            writeAddress(0, 0, pointAddress);
        }
        else if (!state.blocks[s].empty())
        {
            writeAddress(s, 0, state.address[s]);
        }
        for (const BoundBlock& one : blocks[s])
        {
            writeAddress(s, one.index, one.address);
        }
    }

    // Each attribute's stream on buffer 16 + type, at its offset in the vertex buffer's memory as it
    // is at the draw.
    if (attributes != 0)
    {
        const MetalAllocation memory = metalAllocation(vertices);
        const MTLGPUAddress base = memory.buffer.gpuAddress + memory.offset;
        for (U32 type = 0; type < VertexStreams::MAX_ATTRIBUTES; ++type)
        {
            if (attributes & (1u << type))
            {
                const U64 offset = type == TYPE_TEXTURE_INDEX ? streams.offset[TYPE_VERTEX] + 12 : streams.offset[type];
                writeAddress(0, METAL_FIRST_VERTEX_BUFFER + type, base + offset);
            }
        }
    }

    for (size_t s = 0; s < 2; ++s)
    {
        for (const Sampled& one : sampled[s])
        {
            writeTexture(s, one.index, one.texture);
            writeSampler(s, one.index, one.sampler);
        }
    }
    return true;
}

bool MetalEncoder::bindNativeStage(const char* call, NativeStage stage, size_t table)
{
    const auto refuse = [call](const std::string& why)
    {
        LL_WARNS("RHI") << call << ": " << why << LL_ENDL;
        return false;
    };
    const bool checks = mDevice.debugChecks();
    MetalResidency& residency = mDevice.residency();
    // Each texture as the view of the levels its binding covers (the texture itself for all of them), resident through
    // the texture it views; checked with the Metal debug setting before anything is written.
    struct Bound
    {
        U32 slot;
        MTLResourceID id;
    };
    std::vector<Bound> textures;
    for (const auto& [slot, binding] : mTracker.nativeTextures(stage))
    {
        const auto levels = rangeLevels(binding.range, binding.texture->desc().mipLevels);
        IdMTLTexture view = metalLevelView(binding.texture.get(), levels.first, levels.second);
        if (!view)
        {
            return refuse("Metal gave no view of texture '" + binding.texture->debugName() + "''s levels " +
                          std::to_string(levels.first) + "-" + std::to_string(levels.second - 1));
        }
        if (checks && !residency.containsTexture(view))
        {
            return refuse("texture '" + binding.texture->debugName() + "' is not resident (native slot " +
                          std::to_string(slot) + ")");
        }
        textures.push_back({ slot, view.gpuResourceID });
    }
    struct Address
    {
        U32 slot;
        MTLGPUAddress address;
    };
    std::vector<Address> addresses;
    for (const auto& [slot, binding] : mTracker.nativeBuffers(stage))
    {
        const MetalAllocation memory = metalAllocation(binding.buffer.get());
        if (checks && !residency.contains(memory.buffer))
        {
            return refuse("buffer '" + binding.buffer->debugName() + "' is not resident (native slot " +
                          std::to_string(slot) + ")");
        }
        addresses.push_back({ slot, memory.buffer.gpuAddress + memory.offset + binding.offset });
    }
    const NativeSlots& slots = mNativeSlots[size_t(stage)];
    for (U32 slot = 0; slot < NATIVE_BUFFER_SLOTS; ++slot)
    {
        if (slots.bytes[slot] != 0)
        {
            addresses.push_back({ slot, slots.bytes[slot] });
        }
    }
    std::vector<Bound> samplers;
    for (U32 slot = 0; slot < NATIVE_SAMPLER_SLOTS; ++slot)
    {
        if (slots.samplers[slot])
        {
            Ref<Sampler> sampler = mDevice.getSampler(*slots.samplers[slot]);
            if (sampler.isNull())
            {
                return false; // getSampler wrote the WARN line
            }
            samplers.push_back({ slot, metalSampler(sampler.get()).gpuResourceID });
        }
    }
    if (!makeTables(call))
    {
        return false;
    }
    for (const Address& one : addresses)
    {
        writeAddress(table, one.slot, one.address);
    }
    for (const Bound& one : textures)
    {
        writeTexture(table, one.slot, one.id);
    }
    for (const Bound& one : samplers)
    {
        writeSampler(table, one.slot, one.id);
    }
    if (checks)
    {
        // With the Metal debug setting, the native slots this stage does not bind are written empty, so a function
        // that reads one finds nothing rather than what an earlier pass left there, which may be freed.
        std::array<bool, NATIVE_TEXTURE_SLOTS> textured{};
        std::array<bool, NATIVE_BUFFER_SLOTS> addressed{};
        std::array<bool, NATIVE_SAMPLER_SLOTS> sampled{};
        for (const Bound& one : textures)
        {
            textured[one.slot] = true;
        }
        for (const Address& one : addresses)
        {
            addressed[one.slot] = true;
        }
        for (const Bound& one : samplers)
        {
            sampled[one.slot] = true;
        }
        const MTLResourceID none = {};
        for (U32 slot = 0; slot < NATIVE_TEXTURE_SLOTS; ++slot)
        {
            if (!textured[slot])
            {
                writeTexture(table, slot, none);
            }
        }
        for (U32 slot = 0; slot < NATIVE_BUFFER_SLOTS; ++slot)
        {
            if (!addressed[slot])
            {
                writeAddress(table, slot, 0);
            }
        }
        for (U32 slot = 0; slot < NATIVE_SAMPLER_SLOTS; ++slot)
        {
            if (!sampled[slot])
            {
                writeSampler(table, slot, none);
            }
        }
    }
    return true;
}

bool MetalEncoder::bindNativeForDraw(const char* call)
{
    return bindNativeStage(call, NativeStage::Vertex, 0) && bindNativeStage(call, NativeStage::Fragment, 1);
}

bool MetalEncoder::bindForDispatch(const char* call)
{
    return bindNativeStage(call, NativeStage::Compute, 2);
}

} // namespace LL::RHI

#endif // LL_RHI_METAL
