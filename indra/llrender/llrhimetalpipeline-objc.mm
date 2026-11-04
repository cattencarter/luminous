/**
 * @file llrhimetalpipeline-objc.mm
 * @brief The Metal backend's programs and pipelines
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
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <functional>
#include <thread>

#include <pthread.h>

namespace LL::RHI
{

namespace
{
    // The one way to the translator. Constant initialization, so nothing runs for it at load
    // time; null until someone registers MSL::translate.
    std::atomic<MetalTranslator> sTranslator;

    const char* stageName(ShaderStage stage)
    {
        switch (stage)
        {
        case ShaderStage::Vertex: return "vertex";
        case ShaderStage::Fragment: return "fragment";
        case ShaderStage::Geometry: return "geometry";
        }
        return "unknown";
    }

    MSL::Stage mslStage(ShaderStage stage)
    {
        switch (stage)
        {
        case ShaderStage::Vertex: return MSL::Stage::Vertex;
        case ShaderStage::Fragment: return MSL::Stage::Fragment;
        case ShaderStage::Geometry: return MSL::Stage::Geometry;
        }
        return MSL::Stage::Vertex;
    }

    struct NamedType
    {
        const char* name;
        UniformType type;
    };

    // The default block's types as translate() names them (llrhimsl.cpp's typeName); uint and
    // bool are set as Int, as GL's setters set them.
    constexpr NamedType UNIFORM_TYPES[] = {
        { "float", UniformType::Float },      { "float2", UniformType::Vec2 },     { "float3", UniformType::Vec3 },
        { "float4", UniformType::Vec4 },      { "int", UniformType::Int },         { "int2", UniformType::IVec2 },
        { "int3", UniformType::IVec3 },       { "int4", UniformType::IVec4 },      { "uint", UniformType::Int },
        { "uint2", UniformType::IVec2 },      { "uint3", UniformType::IVec3 },     { "uint4", UniformType::IVec4 },
        { "bool", UniformType::Int },         { "bool2", UniformType::IVec2 },     { "bool3", UniformType::IVec3 },
        { "bool4", UniformType::IVec4 },      { "float2x2", UniformType::Mat2 },   { "float3x3", UniformType::Mat3 },
        { "float3x4", UniformType::Mat3x4 },  { "float4x4", UniformType::Mat4 },
    };

    // The samplers' textures as translate() names them (llrhimsl.cpp's textureType).
    constexpr NamedType TEXTURE_TYPES[] = {
        { "2d", UniformType::Sampler2D },
        { "2d_shadow", UniformType::Sampler2DShadow },
        { "cube", UniformType::SamplerCube },
        { "cube_array", UniformType::SamplerCubeArray },
        { "3d", UniformType::Sampler3D },
    };

    template <size_t N>
    bool typeFor(const NamedType (&types)[N], const std::string& name, UniformType& type)
    {
        for (const NamedType& named : types)
        {
            if (name == named.name)
            {
                type = named.type;
                return true;
            }
        }
        return false;
    }

    /// The interface type of a default-block member; false for a type not supported yet.
    bool uniformTypeFor(const std::string& mslType, UniformType& type)
    {
        // translate() writes an array's sizes after the element type (llrhimsl.cpp:221-224); the
        // size is Uniform::arraySize.
        return typeFor(UNIFORM_TYPES, mslType.substr(0, mslType.find('[')), type);
    }

    /// The interface index of a uniform; a second stage that reads more of an array raises its size.
    U32 uniformIndex(ProgramReflection& reflection, const std::string& name, UniformType type, U32 arraySize)
    {
        for (size_t i = 0; i < reflection.uniforms.size(); ++i)
        {
            if (reflection.uniforms[i].name == name)
            {
                reflection.uniforms[i].arraySize = std::max(reflection.uniforms[i].arraySize, arraySize);
                return U32(i);
            }
        }
        reflection.uniforms.push_back({ name, type, std::max(arraySize, 1u) });
        return U32(reflection.uniforms.size() - 1);
    }

    std::string addUniforms(const MSL::Reflection& source, ProgramReflection& reflection, MetalStageLayout& stage)
    {
        stage.blockSize = source.defaultBlockSize;
        for (const MSL::Uniform& uniform : source.uniforms) // by offset
        {
            if (!uniform.active)
            {
                continue; // GL gives it no location; its place in this stage has size 0
            }
            UniformType type;
            if (!uniformTypeFor(uniform.type, type))
            {
                return "uniform " + uniform.name + " is a " + uniform.type + ", which comes with P3-04a";
            }
            const U32 index = uniformIndex(reflection, uniform.name, type, std::max(uniform.activeArraySize, 1u));
            // The place keeps the declared array: the MSL's struct has it whole.
            stage.places.resize(std::max<size_t>(stage.places.size(), index + 1));
            stage.places[index] = { uniform.offset, uniform.arrayStride, uniform.matrixStride, uniform.size };
        }
        return {};
    }

    std::string addTextures(const MSL::Reflection& source, ProgramReflection& reflection, MetalStageLayout& stage)
    {
        for (const MSL::Texture& texture : source.textures) // by MSL index
        {
            UniformType type;
            if (!typeFor(TEXTURE_TYPES, texture.type, type))
            {
                return "sampler " + texture.name + " has a " + texture.type + " texture, which comes with P3-04a";
            }
            const U32 index = uniformIndex(reflection, texture.name, type, 1);
            stage.textureUniforms.resize(std::max<size_t>(stage.textureUniforms.size(), texture.index + 1));
            stage.textureUniforms[texture.index] = index;
            stage.textureTypes.resize(stage.textureUniforms.size(), UniformType::Sampler2D);
            stage.textureTypes[texture.index] = type;
        }
        return {};
    }

    /// The stage's blocks, once per name, at their slot's bit (buffer 1 + slot).
    std::string addBlocks(const MSL::Reflection& source, std::span<const char* const> blocks, ProgramReflection& reflection,
                          MetalStageLayout& stage)
    {
        for (const MSL::Block& block : source.blocks)
        {
            const U32 slot = block.buffer - 1;
            if (block.buffer == 0 || slot >= blocks.size() || block.name != blocks[slot])
            {
                return "uniform block " + block.name + " in buffer " + std::to_string(block.buffer) +
                       " is not the program's block for that slot";
            }
            stage.uniformBlocks |= 1u << slot;
            if (stage.blockSizes.size() <= slot)
            {
                stage.blockSizes.resize(slot + 1, 0);
            }
            stage.blockSizes[slot] = block.size;
            const auto same = [&block](const UniformBlockInfo& info) { return info.name == block.name; };
            if (std::none_of(reflection.blocks.begin(), reflection.blocks.end(), same))
            {
                reflection.blocks.push_back({ block.name, block.size });
            }
        }
        return {};
    }

    /// Metal 4's argument tables: 31 buffers, 128 textures and 16 samplers per stage
    /// (MTL4ArgumentTable.h:26-37); texture i has sampler i, so 16 of both.
    constexpr U32 STAGE_TEXTURES = 16;
    /// Buffers 0-7 hold uniforms, the default block and the blocks at 1 + slot.
    constexpr U32 MAX_BLOCK_BUFFER = 7;
    static_assert(METAL_FIRST_VERTEX_BUFFER + VertexStreams::MAX_ATTRIBUTES <= 31, "the streams fit the 31 buffers");

    struct VertexFormat
    {
        MTLVertexFormat format;
        U32 stride;
    };

    // LLVertexBuffer's streams by attribute type, as setupVertexAttributes gives them to
    // GL (llvertexbuffer.cpp:2683) with sTypeSize's strides (llvertexbuffer.cpp:1343-1359).
    constexpr VertexFormat VERTEX_FORMATS[VertexStreams::MAX_ATTRIBUTES] = {
        { MTLVertexFormatFloat3, 16 },           // TYPE_VERTEX
        { MTLVertexFormatFloat3, 16 },           // TYPE_NORMAL
        { MTLVertexFormatFloat2, 8 },            // TYPE_TEXCOORD0
        { MTLVertexFormatFloat2, 8 },            // TYPE_TEXCOORD1
        { MTLVertexFormatFloat2, 8 },            // TYPE_TEXCOORD2
        { MTLVertexFormatFloat2, 8 },            // TYPE_TEXCOORD3
        { MTLVertexFormatUChar4Normalized, 4 },  // TYPE_COLOR
        { MTLVertexFormatUChar4Normalized, 4 },  // TYPE_EMISSIVE
        { MTLVertexFormatFloat4, 16 },           // TYPE_TANGENT
        { MTLVertexFormatFloat, 4 },             // TYPE_WEIGHT
        { MTLVertexFormatFloat4, 16 },           // TYPE_WEIGHT4
        { MTLVertexFormatFloat4, 16 },           // TYPE_CLOTHWEIGHT
        { MTLVertexFormatUShort4, 8 },           // TYPE_JOINT
        { MTLVertexFormatInt, 16 },              // TYPE_TEXTURE_INDEX
    };
    // TYPE_TEXTURE_INDEX: GL takes it as GL_UNSIGNED_INT (glVertexAttribIPointer), and the shaders read
    // `in int texture_index` (objects/indexedTextureV.glsl); GL gives the bits, while Metal refuses
    // UInt for an int attribute (a run-time probe), so it is Int: the same bytes for any index.

    MTLColorWriteMask writeMask(U8 mask)
    {
        MTLColorWriteMask metal = MTLColorWriteMaskNone;
        metal |= (mask & COLOR_MASK_R) ? MTLColorWriteMaskRed : MTLColorWriteMaskNone;
        metal |= (mask & COLOR_MASK_G) ? MTLColorWriteMaskGreen : MTLColorWriteMaskNone;
        metal |= (mask & COLOR_MASK_B) ? MTLColorWriteMaskBlue : MTLColorWriteMaskNone;
        metal |= (mask & COLOR_MASK_A) ? MTLColorWriteMaskAlpha : MTLColorWriteMaskNone;
        return metal;
    }
}

void setMetalTranslator(MetalTranslator translator)
{
    sTranslator.store(translator);
}

bool MetalMathKnobs::safe(const std::string& program) const
{
    return safeAll || std::find(safeNames.begin(), safeNames.end(), program) != safeNames.end();
}

std::string MetalMathKnobs::describe() const
{
    std::string text = "safe math ";
    if (safeAll)
    {
        text += "all";
    }
    else if (safeNames.empty())
    {
        text += "none";
    }
    for (size_t i = 0; !safeAll && i < safeNames.size(); ++i)
    {
        text += (i ? ", '" : "'") + safeNames[i] + "'";
    }
    text += precise ? " with precise functions" : "";
    text += invariant ? "; invariant position" : "";
    return text;
}

const MetalMathKnobs& metalMathKnobs()
{
    // A function-local static, made at the first use (nothing at load time).
    static const MetalMathKnobs knobs = []()
    {
        MetalMathKnobs read;
        const char* safe = getenv("LL_METAL_SAFE_MATH");
        if (safe && std::strcmp(safe, "all") == 0)
        {
            read.safeAll = true;
        }
        else if (safe)
        {
            std::string names(safe);
            for (size_t start = 0; start <= names.size();)
            {
                const size_t end = std::min(names.find(',', start), names.size());
                if (end > start)
                {
                    read.safeNames.push_back(names.substr(start, end - start));
                }
                start = end + 1;
            }
        }
        const char* precise = getenv("LL_METAL_PRECISE_MATH");
        read.precise = precise && std::strcmp(precise, "1") == 0;
        const char* invariant = getenv("LL_METAL_INVARIANT");
        read.invariant = invariant && std::strcmp(invariant, "1") == 0;
        return read;
    }();
    return knobs;
}

bool metalCompileSlowKnob()
{
    // A function-local static, made at the first use (nothing at load time).
    static const bool slow = []()
    {
        const char* knob = getenv("LL_METAL_COMPILE_SLOW");
        return knob && std::strcmp(knob, "1") == 0;
    }();
    return slow;
}

std::vector<std::string> metalSafeMathPrograms()
{
    return std::vector<std::string>(METAL_SAFE_MATH_PROGRAMS.begin(), METAL_SAFE_MATH_PROGRAMS.end());
}

bool metalSafeMath(const std::string& program)
{
    constexpr std::string_view skinned = "Skinned "; // make_rigged_variant's name for a skinned variant
    const std::string_view name =
        std::string_view(program).starts_with(skinned) ? std::string_view(program).substr(skinned.size()) : program;
    return std::find(METAL_SAFE_MATH_PROGRAMS.begin(), METAL_SAFE_MATH_PROGRAMS.end(), name)
               != METAL_SAFE_MATH_PROGRAMS.end()
           || metalMathKnobs().safe(program);
}

namespace
{
    /// [Viewer Lab] Golden level 2's fault list on Metal, kept as llrhi.cpp keeps the shared fault: each entry with
    /// its hits and the creations of its program taken since one built. A function's static, so nothing runs for it
    /// at load. Main thread.
    struct MetalProgramFaultList
    {
        std::vector<ProgramFault> faults;
        std::vector<U32> hits;
        std::vector<U32> inRow;
    };

    MetalProgramFaultList& metalProgramFaultList()
    {
        static MetalProgramFaultList list;
        return list;
    }
}

void setMetalProgramFaults(const std::vector<ProgramFault>& faults)
{
    MetalProgramFaultList& list = metalProgramFaultList();
    list.faults = faults;
    list.hits.assign(faults.size(), 0);
    list.inRow.assign(faults.size(), 0);
}

U32 metalProgramFaultHits(const std::string& program)
{
    const MetalProgramFaultList& list = metalProgramFaultList();
    for (size_t i = 0; i < list.faults.size(); ++i)
    {
        if (!list.faults[i].program.empty() && list.faults[i].program == program)
        {
            return list.hits[i];
        }
    }
    return 0;
}

bool takeMetalProgramFault(const std::string& program, bool& deferred)
{
    // takeProgramFault's rule per entry (llrhi.cpp); the first entry that names the program decides
    MetalProgramFaultList& list = metalProgramFaultList();
    for (size_t i = 0; i < list.faults.size(); ++i)
    {
        const ProgramFault& fault = list.faults[i];
        if (fault.program.empty() || program != fault.program)
        {
            continue;
        }
        if (fault.times > 0 && list.inRow[i] >= fault.times)
        {
            list.inRow[i] = 0; // this creation builds, and the next load fails its first ones again
            return false;
        }
        ++list.inRow[i];
        ++list.hits[i];
        deferred = fault.deferred;
        return true;
    }
    return false;
}

MTLCompileOptions* metalCompileOptions(bool safe)
{
    // Metal 3's options: MTLLibrary.h:344 with MTLLanguageVersion4_0 (222), and the math
    // set explicitly (329-338) instead of the deprecated fastMathEnabled (323-326).
    MTLCompileOptions* options = [MTLCompileOptions new];
    options.languageVersion = MTLLanguageVersion4_0;
    options.mathMode = safe ? MTLMathModeSafe : METAL_MATH_MODE;
    options.mathFloatingPointFunctions =
        safe && metalMathKnobs().precise ? MTLMathFloatingPointFunctionsPrecise : METAL_MATH_FUNCTIONS;
    // The invariant position of LL_METAL_INVARIANT holds only with this (MTLLibrary.h:383-386).
    options.preserveInvariance = metalMathKnobs().invariant;
    return options;
}

MSL::Program metalProgramSource(const ProgramDesc& desc)
{
    MSL::Program program;
    program.name = desc.name;
    for (const StageSource& stage : desc.stages)
    {
        for (size_t i = 0; i < stage.units.size(); ++i)
        {
            // The unit's file and classes for the translator's log and the disk cache key. An own
            // unit was asked and compiled at the attempt's level and found in its file's class; a shared object's
            // classes are its own (0, not known); an internal program's unit has no file.
            MSL::Unit unit;
            const bool named = i < stage.files.size() && !stage.files[i].empty();
            const S32 found = i < stage.classes.size() ? stage.classes[i] : 0;
            unit.file = named ? stage.files[i] : std::string(stageName(stage.stage)) + " unit " + std::to_string(i);
            unit.stage = mslStage(stage.stage);
            unit.source = stage.units[i];
            unit.requestedClass = found > 0 ? desc.shaderLevel : 0;
            unit.compiledClass = unit.requestedClass;
            unit.usedClass = found;
            program.units.push_back(std::move(unit));
        }
    }
    program.attributes = desc.reservedAttributes;
    program.blocks.assign(desc.uniformBlocks.begin(), desc.uniformBlocks.end());
    return program;
}

std::string metalGroupSource(std::span<const std::array<std::string, 2>> stages)
{
    // A unit test compiles this form with the UI and the packing programs' MSL.
    const auto lifted = [](std::string_view line)
    {
        return line.starts_with("#include") || line.starts_with("#pragma") || line == "using namespace metal;";
    };
    std::vector<std::string> head;
    std::string body;
    for (size_t i = 0; i < stages.size(); ++i)
    {
        for (size_t stage = 0; stage < 2; ++stage)
        {
            body += std::string("namespace ") + (stage == 0 ? "v" : "f") + std::to_string(i) + "\n{\n";
            const std::string_view msl = stages[i][stage];
            for (size_t at = 0; at < msl.size();)
            {
                const size_t end = std::min(msl.find('\n', at), msl.size());
                const std::string_view line = msl.substr(at, end - at);
                if (!lifted(line))
                {
                    body.append(line).append("\n");
                }
                else if (std::find(head.begin(), head.end(), line) == head.end())
                {
                    head.emplace_back(line);
                }
                at = end + 1;
            }
            body += "}\n\n";
        }
    }
    std::string source;
    for (const std::string& line : head)
    {
        source += line + "\n";
    }
    return source + "\n" + body;
}

std::string metalStageLimits(const MSL::StageResult& stage)
{
    U32 textures = 0;
    for (const MSL::Texture& texture : stage.reflection.textures)
    {
        textures = std::max(textures, texture.index + 1);
    }
    if (textures > STAGE_TEXTURES)
    {
        // Not MSL::stageName: the backend calls nothing of llrhimsl.cpp.
        return std::string("the ") + (stage.stage == MSL::Stage::Vertex ? "vertex" : "fragment") + " stage uses " +
               std::to_string(textures) + " textures and samplers; Metal's argument tables bind " +
               std::to_string(STAGE_TEXTURES) + " (decision 4: 128 textures, 16 samplers)";
    }
    for (const MSL::Block& block : stage.reflection.blocks)
    {
        if (block.buffer > MAX_BLOCK_BUFFER)
        {
            return "uniform block " + block.name + " is in buffer " + std::to_string(block.buffer) +
                   "; decision 4 has buffers 0-" + std::to_string(MAX_BLOCK_BUFFER) + " for uniforms";
        }
    }
    return {};
}

std::string metalReflection(const MSL::Result& result, std::span<const char* const> blocks, ProgramReflection& reflection,
                            MetalProgramLayout& layout)
{
    reflection = ProgramReflection();
    layout = MetalProgramLayout();
    const MSL::StageResult* vertex = nullptr;
    const MSL::StageResult* fragment = nullptr;
    for (const MSL::StageResult& stage : result.stages)
    {
        const std::string limits = metalStageLimits(stage);
        if (!limits.empty())
        {
            return limits;
        }
        (stage.stage == MSL::Stage::Vertex ? vertex : fragment) = &stage;
    }
    if (!vertex || !fragment)
    {
        return "the program needs a vertex and a fragment stage";
    }
    for (const MSL::Variable& input : vertex->inputs)
    {
        if (input.location >= VertexStreams::MAX_ATTRIBUTES)
        {
            return "vertex input " + input.name + " is not one of LLVertexBuffer's attributes";
        }
        reflection.attributeMask |= 1u << input.location;
    }
    std::string why = addUniforms(vertex->reflection, reflection, layout.vertex);
    why = why.empty() ? addUniforms(fragment->reflection, reflection, layout.fragment) : why;
    why = why.empty() ? addTextures(fragment->reflection, reflection, layout.fragment) : why;
    why = why.empty() ? addTextures(vertex->reflection, reflection, layout.vertex) : why;
    why = why.empty() ? addBlocks(vertex->reflection, blocks, reflection, layout.vertex) : why;
    why = why.empty() ? addBlocks(fragment->reflection, blocks, reflection, layout.fragment) : why;
    // Every uniform has a place in each stage; size 0 where the stage does not use it.
    layout.vertex.places.resize(reflection.uniforms.size());
    layout.fragment.places.resize(reflection.uniforms.size());
    layout.constantData = metalProgramConstants(vertex->msl) || metalProgramConstants(fragment->msl);
    return why;
}

bool metalProgramConstants(const std::string& msl)
{
    constexpr std::string_view declaration = "constant ";
    for (size_t line = 0; line < msl.size();)
    {
        if (std::string_view(msl).substr(line, declaration.size()) == declaration)
        {
            return true;
        }
        const size_t end = msl.find('\n', line);
        line = end == std::string::npos ? msl.size() : end + 1;
    }
    return false;
}

U32 metalVertexStride(U32 type)
{
    return type < VertexStreams::MAX_ATTRIBUTES ? VERTEX_FORMATS[type].stride : 0;
}

MTLVertexDescriptor* metalVertexDescriptor(U32 mask)
{
    MTLVertexDescriptor* descriptor = [MTLVertexDescriptor vertexDescriptor];
    for (U32 i = 0; i < VertexStreams::MAX_ATTRIBUTES; ++i)
    {
        if (mask & (1u << i))
        {
            const U32 buffer = METAL_FIRST_VERTEX_BUFFER + i;
            descriptor.attributes[i].format = VERTEX_FORMATS[i].format;
            descriptor.attributes[i].offset = 0;
            descriptor.attributes[i].bufferIndex = buffer;
            descriptor.layouts[buffer].stride = VERTEX_FORMATS[i].stride;
            descriptor.layouts[buffer].stepFunction = MTLVertexStepFunctionPerVertex;
        }
    }
    return descriptor;
}

// Programs ---------------------------------------------------------------------------------------

/// A job's phases. The result is written before the release-store of Ready or Failed.
enum class MetalJobPhase : U8
{
    Queued,
    Translating,
    Translated,
    Compiling,
    Ready,
    Failed,
};

struct API_AVAILABLE(macos(26.0)) MetalProgramJob
{
    /// The copy is made in place: an MSL::Program assigned or moved would be translator code in the
    /// viewer, which only the structs' destructors may be.
    explicit MetalProgramJob(const ProgramDesc& desc) : program(metalProgramSource(desc)) {}
    /// The point variant of a Ready program's job, which translates the parent's
    /// MSL::Program by reference with Options::pointSize (no copy).
    explicit MetalProgramJob(std::shared_ptr<MetalProgramJob> of) : parent(std::move(of)) {}

    /// What the translator gets: the parent's program for a point variant.
    const MSL::Program& source() const { return parent ? parent->program : program; }
    /// The name in WARN lines and library labels.
    std::string name() const { return parent ? parent->program.name + " (points)" : program.name; }

    const std::shared_ptr<MetalProgramJob> parent; ///< the program a point variant is of; null otherwise
    /// Copied from the ProgramDesc on the calling thread; empty in a point variant.
    const MSL::Program program;
    MetalTranslator translator = nullptr; ///< read on the calling thread
    IdMTL4Compiler compiler = nil;        ///< made on the main thread before the first job
    std::shared_ptr<MetalCompileState> state;
    std::shared_ptr<MetalProgramGroup> group; ///< set in createProgram; none for a login program
    MetalProgramGroups* groups = nullptr;     ///< the device's, which joins the pool before it goes
    U64 seq = 0;               ///< its place in the pool's queue; 0 for a login program
    bool failCompile = false;  ///< failNextCompile
    U32 sleepHandlerMs = 0;    ///< sleepNextHandlerMs
    std::atomic<MetalJobPhase> phase{ MetalJobPhase::Queued };
    std::atomic<bool> cancelled{ false }; ///< the program is gone; the job ends quietly

    // The result, written under mutex; read-only after Ready or Failed.
    std::mutex mutex;
    std::condition_variable changed;
    ProgramReflection reflection;
    MetalProgramLayout layout;
    IdMTLLibrary libraries[2] = { nil, nil }; ///< vertex, fragment; a group's members hold its library twice
    std::array<std::string, 2> msl;           ///< vertex, fragment; written before Translated
    std::string varyings; ///< the vertex stage's outputs, which a point variant must keep; before Translated
    MetalProgramFunctions functions;
    U32 librariesLeft = 2;
    std::string error;
};

/// A group's members in createProgram's order. Under MetalProgramGroups' mutex. A member holds
/// its group; the group holds its members weakly, so a released member that is gone is left out.
struct API_AVAILABLE(macos(26.0)) MetalProgramGroup
{
    U64 number = 0;
    std::vector<std::weak_ptr<MetalProgramJob>> members;
    size_t untranslated = 0; ///< members not yet translated, failed or cancelled
    bool closed = false;
    bool sent = false;
};

namespace
{
    bool finished(MetalJobPhase phase)
    {
        return phase == MetalJobPhase::Ready || phase == MetalJobPhase::Failed;
    }

    /// Adds the nanoseconds since start to one of MetalCompileState's times.
    void addTimeSince(std::atomic<U64>& total, std::chrono::steady_clock::time_point start)
    {
        const auto elapsed = std::chrono::steady_clock::now() - start;
        total.fetch_add(U64(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()),
                        std::memory_order_relaxed);
    }

    /// Raises total to value when value is larger (the longest waits).
    template <typename T> void raiseTo(std::atomic<T>& total, T value)
    {
        T seen = total.load(std::memory_order_relaxed);
        while (seen < value && !total.compare_exchange_weak(seen, value, std::memory_order_relaxed))
        {
        }
    }

    /// A calling thread's wait since start, in its total (program, point or pipeline) and by its kind, with the
    /// longest single wait.
    void countWait(MetalCompileState& state, MetalWaitKind kind, std::chrono::steady_clock::time_point start)
    {
        const U64 ns = U64(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start)
                               .count());
        std::atomic<U64>& total = kind == MetalWaitKind::Program ? state.programWaitNs
                                  : kind == MetalWaitKind::Point ? state.pointWaitNs
                                                                 : state.pipelineWaitNs;
        total.fetch_add(ns, std::memory_order_relaxed);
        const size_t index = size_t(kind);
        state.waitCount[index].fetch_add(1, std::memory_order_relaxed);
        state.waitKindNs[index].fetch_add(ns, std::memory_order_relaxed);
        raiseTo(state.longestWaitNs[index], ns);
    }

    void setPhase(MetalProgramJob& job, MetalJobPhase phase) API_AVAILABLE(macos(26.0))
    {
        std::lock_guard<std::mutex> lock(job.mutex);
        job.phase.store(phase, std::memory_order_release);
        job.changed.notify_all();
    }

    /// Failed with one WARN line; a cancelled job ends quietly. The line is held for the main thread
    /// (MetalFailureLog) before Failed is stored, so a wait() that returns after it writes it.
    void failJob(MetalProgramJob& job, const std::string& why) API_AVAILABLE(macos(26.0))
    {
        std::lock_guard<std::mutex> lock(job.mutex);
        if (finished(job.phase.load(std::memory_order_relaxed)))
        {
            return; // the other library's handler said it first
        }
        if (!job.cancelled.load())
        {
            const std::string line = "createProgram " + job.name() + ": " + why;
            if (job.state)
            {
                job.state->failures->add(MetalFailureLog::Kind::Program, line);
                job.state->failedPrograms.fetch_add(1, std::memory_order_relaxed);
            }
            else
            {
                LL_WARNS("RHI") << line << LL_ENDL;
            }
        }
        job.error = why;
        job.phase.store(MetalJobPhase::Failed, std::memory_order_release);
        job.changed.notify_all();
    }

    MTL4FunctionDescriptor* mainFunction(IdMTLLibrary library, const std::string& name = "main0")
        API_AVAILABLE(macos(26.0))
    {
        // The translator's entry point (MTL4LibraryFunctionDescriptor.h:22-28); in a group's
        // library inside the member's namespaces.
        MTL4LibraryFunctionDescriptor* descriptor = [MTL4LibraryFunctionDescriptor new];
        descriptor.name = metalString(name);
        descriptor.library = library;
        return descriptor;
    }

    /// One library's handler: Ready with the second library, Failed without a library (also when
    /// the compiler's connection broke, MTLLibrary.h:456-463; no second try).
    void libraryDone(MetalProgramJob& job, size_t index, IdMTLLibrary library, const std::string& error)
        API_AVAILABLE(macos(26.0))
    {
        if (!library)
        {
            failJob(job, std::string("the ") + (index == 0 ? "vertex" : "fragment") + " stage does not compile: " + error);
            return;
        }
        std::lock_guard<std::mutex> lock(job.mutex);
        if (job.cancelled.load() || finished(job.phase.load(std::memory_order_relaxed)))
        {
            return; // the result is dropped with the block
        }
        job.libraries[index] = library;
        if (--job.librariesLeft == 0)
        {
            job.functions.vertex = mainFunction(job.libraries[0]);
            job.functions.fragment = mainFunction(job.libraries[1]);
            job.phase.store(MetalJobPhase::Ready, std::memory_order_release);
            job.changed.notify_all();
        }
    }

    /// Sends both libraries at once (MTL4Compiler.h:238-239). Not newLibraryWithDescriptor:error:
    /// (MTL4Compiler.h:100-108): under Metal API Validation the error it gives back is already freed,
    /// and taking it crashes in objc_retain (a run-time observation). The handler's error is valid
    /// while the handler runs. waitUntilCompleted waits for the task only (MTL4CompilerTask.h:47-48),
    /// not for the handler, so MetalCompileState counts the handlers.
    /// `counted`: a group's fall-back has counted both handlers already, and sends them also for a
    /// member released since (its handlers drop the result).
    void compileJob(const std::shared_ptr<MetalProgramJob>& job, bool counted) API_AVAILABLE(macos(26.0))
    {
        if (!counted && job->cancelled.load())
        {
            return;
        }
        setPhase(*job, MetalJobPhase::Compiling);
        U32 sleepMs = job->sleepHandlerMs;
        for (size_t index = 0; index < job->msl.size(); ++index)
        {
            const bool vertex = index == 0;
            // MTL4LibraryDescriptor.h:26-32.
            MTL4LibraryDescriptor* source = [MTL4LibraryDescriptor new];
            source.source = metalString(job->msl[index]);
            // The Safe-math list and knob, by the program's name (a point variant's is its program's)
            source.options = metalCompileOptions(metalSafeMath(job->source().name));
            source.name = metalString(job->name() + (vertex ? " vertex" : " fragment"));
            const std::shared_ptr<MetalCompileState> state = job->state;
            if (!counted)
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                ++state->outstanding; // before it is sent
            }
            // The handler holds the job and the state only, and lets go of the job before it
            // counts itself down, so nothing of the job outlives ~MetalDevice's wait.
            __block std::shared_ptr<MetalProgramJob> held = job;
            const U32 sleepFor = std::exchange(sleepMs, 0u); // the first handler only
            [job->compiler newLibraryWithDescriptor:source completionHandler:^(id<MTLLibrary> library, NSError* error)
            {
                if (sleepFor)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(sleepFor));
                }
                libraryDone(*held, index, library,
                            library ? std::string() : (error ? error.localizedDescription.UTF8String : "no error given"));
                held.reset();
                std::lock_guard<std::mutex> lock(state->mutex);
                state->handlersAfterClose += state->closing ? 1 : 0;
                --state->outstanding;
                state->changed.notify_all();
            }];
        }
    }

    /// A group whose library failed sends each member that is still wanted alone, with
    /// both its libraries as compileJob sends them. Counted before they are sent, from the group's handler,
    /// which is still outstanding; refused once the device has stopped waiting. No WARN line of
    /// its own: only the members' own failures write one.
    void fallBack(const std::vector<std::shared_ptr<MetalProgramJob>>& members) API_AVAILABLE(macos(26.0))
    {
        const std::shared_ptr<MetalCompileState> state = members.front()->state;
        std::vector<std::shared_ptr<MetalProgramJob>> alone;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->closing)
            {
                return; // the device is gone; nothing is sent after its wait
            }
            ++state->groupFallBacks;
            for (const std::shared_ptr<MetalProgramJob>& job : members)
            {
                if (!job->cancelled.load())
                {
                    alone.push_back(job);
                    state->outstanding += job->msl.size();
                }
            }
        }
        for (const std::shared_ptr<MetalProgramJob>& job : alone)
        {
            compileJob(job, true);
        }
    }

    /// A group's library is made. Each member that is still wanted holds it and is Ready with
    /// its namespaces' functions; without a library the members go alone (fallBack).
    void groupDone(const std::vector<std::shared_ptr<MetalProgramJob>>& members, IdMTLLibrary library)
        API_AVAILABLE(macos(26.0))
    {
        if (!library)
        {
            fallBack(members);
            return;
        }
        for (size_t i = 0; i < members.size(); ++i)
        {
            MetalProgramJob& job = *members[i];
            std::lock_guard<std::mutex> lock(job.mutex);
            if (job.cancelled.load() || finished(job.phase.load(std::memory_order_relaxed)))
            {
                continue; // the result is dropped with the block
            }
            job.libraries[0] = library;
            job.libraries[1] = library;
            job.functions.vertex = mainFunction(library, "v" + std::to_string(i) + "::main0");
            job.functions.fragment = mainFunction(library, "f" + std::to_string(i) + "::main0");
            job.librariesLeft = 0;
            job.phase.store(MetalJobPhase::Ready, std::memory_order_release);
            job.changed.notify_all();
        }
    }

    /// Sends one library for the group's translated members, in their order, with a member's options
    /// (MTL4Compiler.h:238-239). The handler is counted in MetalCompileState like a member's own.
    void sendGroup(const std::vector<std::shared_ptr<MetalProgramJob>>& members, const std::string& text, U64 number)
        API_AVAILABLE(macos(26.0))
    {
        @autoreleasepool // also on the pool's threads
        {
            U32 sleepFor = 0;
            for (const std::shared_ptr<MetalProgramJob>& job : members)
            {
                setPhase(*job, MetalJobPhase::Compiling);
                sleepFor = std::max(sleepFor, job->sleepHandlerMs);
            }
            MTL4LibraryDescriptor* source = [MTL4LibraryDescriptor new];
            source.source = metalString(text);
            source.options = metalCompileOptions(metalMathKnobs().safeAll); // a listed program is never in a group
            source.name = metalString("program group " + std::to_string(number));
            const std::shared_ptr<MetalCompileState> state = members.front()->state;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                ++state->outstanding; // before it is sent
            }
            // Who sends the group, and the handler's latency and QoS (whether Metal's compilation follows the
            // sender's QoS)
            state->groupsSent.fetch_add(1, std::memory_order_relaxed);
            state->groupsFromMain.fetch_add(pthread_main_np() ? 1 : 0, std::memory_order_relaxed);
            const auto sent = std::chrono::steady_clock::now();
            __block std::vector<std::shared_ptr<MetalProgramJob>> held = members;
            [members.front()->compiler newLibraryWithDescriptor:source
                                              completionHandler:^(id<MTLLibrary> library, NSError*)
            {
                const U64 latency = U64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                            std::chrono::steady_clock::now() - sent).count());
                state->groupLatencyNs.fetch_add(latency, std::memory_order_relaxed);
                raiseTo(state->groupLatencyMaxNs, latency);
                state->groupHandlerQos.fetch_or(metalQosBit(U32(qos_class_self())), std::memory_order_relaxed);
                state->groupsDone.fetch_add(1, std::memory_order_relaxed);
                if (sleepFor)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(sleepFor));
                }
                groupDone(held, library);
                held.clear();
                std::lock_guard<std::mutex> lock(state->mutex);
                state->handlersAfterClose += state->closing ? 1 : 0;
                --state->outstanding;
                state->changed.notify_all();
            }];
        }
    }

    /// Translates the job on this thread, with the translator and the copy it was made with,
    /// and sends its libraries, or, for a member of a group, tells the group. A cancelled job
    /// is never translated.
    void translateJob(const std::shared_ptr<MetalProgramJob>& job) API_AVAILABLE(macos(26.0))
    {
        if (job->cancelled.load())
        {
            if (job->group)
            {
                job->groups->translated(*job);
            }
            return;
        }
        @autoreleasepool // the pool's threads have none of their own
        {
            setPhase(*job, MetalJobPhase::Translating);
            const MSL::Program& source = job->source();
            const MSL::Result result =
                job->translator(source, MSL::Options{ .pointSize = job->parent != nullptr,
                                                      .invariantPosition = metalMathKnobs().invariant });
            std::vector<const char*> blocks;
            for (const std::string& block : source.blocks)
            {
                blocks.push_back(block.c_str());
            }
            ProgramReflection reflection;
            MetalProgramLayout layout;
            std::string why = result.ok ? metalReflection(result, blocks, reflection, layout) : result.error;
            // The vertex stage's outputs by location, name, type and interpolation; a point
            // variant's must be its program's, whose fragment function it is drawn with.
            std::string varyings;
            for (const MSL::StageResult& stage : result.stages)
            {
                if (stage.stage != MSL::Stage::Vertex)
                {
                    continue;
                }
                for (const MSL::Variable& output : stage.outputs)
                {
                    varyings += std::to_string(output.location) + " " + output.name + " " + output.type +
                                (output.flat ? " flat;" : ";");
                }
            }
            if (job->parent)
            {
                std::lock_guard<std::mutex> lock(job->state->mutex);
                ++job->state->pointTranslations;
            }
            if (why.empty() && job->parent && varyings != job->parent->varyings)
            {
                why = "the point variant's vertex outputs (" + varyings + ") are not the program's (" +
                      job->parent->varyings + ")";
            }
            if (!why.empty())
            {
                failJob(*job, why);
                if (job->group)
                {
                    job->groups->translated(*job); // left out of its group
                }
                return;
            }
            if (!job->parent)
            {
                // One line per program on Metal with each stage's files and the classes they compiled at and
                // were found in, held for the main thread (MetalCompileState::classLines); the GL start has none.
                // Not MSL::stageName.
                std::string classes = "Program '" + job->name() + "' on Metal: ";
                const size_t head = classes.size();
                for (const MSL::StageResult& stage : result.stages)
                {
                    classes += std::string(classes.size() == head ? "" : " | ") +
                               (stage.stage == MSL::Stage::Vertex ? "vertex: " : "fragment: ");
                    for (size_t i = 0; i < stage.classes.size(); ++i)
                    {
                        classes += (i ? "; " : "") + stage.classes[i];
                    }
                }
                std::lock_guard<std::mutex> lock(job->state->mutex);
                job->state->classLines.push_back(std::move(classes));
            }
            {
                std::lock_guard<std::mutex> lock(job->mutex);
                job->reflection = std::move(reflection);
                job->layout = std::move(layout);
                job->varyings = std::move(varyings);
                for (const MSL::StageResult& stage : result.stages)
                {
                    const bool vertex = stage.stage == MSL::Stage::Vertex;
                    job->msl[vertex ? 0 : 1] =
                        vertex && job->failCompile ? std::string("this is not MSL (failNextCompile)") : stage.msl;
                }
                job->phase.store(MetalJobPhase::Translated, std::memory_order_release);
                job->changed.notify_all();
            }
            if (job->group)
            {
                job->groups->translated(*job);
            }
            else
            {
                compileJob(job, false);
            }
        }
    }
}

void MetalProgramGroups::add(const std::shared_ptr<MetalProgramJob>& job)
{
    std::lock_guard<std::mutex> lock(mMutex);
    if (!mOpen)
    {
        mOpen = std::make_shared<MetalProgramGroup>();
        mOpen->number = mGroups++;
    }
    mOpen->members.push_back(job);
    ++mOpen->untranslated;
    job->group = mOpen;
    job->groups = this;
    if (mOpen->members.size() == MAX_MEMBERS)
    {
        closeLocked(mOpen); // the new member is not translated yet, so nothing is sent here
    }
}

void MetalProgramGroups::closeLocked(const std::shared_ptr<MetalProgramGroup>& group)
{
    group->closed = true;
    if (mOpen == group)
    {
        mOpen.reset();
    }
}

void MetalProgramGroups::close(const std::shared_ptr<MetalProgramGroup>& group)
{
    std::unique_lock<std::mutex> lock(mMutex);
    if (group->closed)
    {
        return;
    }
    closeLocked(group);
    submitIfTranslated(lock, group);
}

void MetalProgramGroups::closeOpen()
{
    std::unique_lock<std::mutex> lock(mMutex);
    if (!mOpen)
    {
        return;
    }
    const std::shared_ptr<MetalProgramGroup> group = mOpen;
    closeLocked(group);
    submitIfTranslated(lock, group);
}

std::vector<std::shared_ptr<MetalProgramJob>> MetalProgramGroups::members(const std::shared_ptr<MetalProgramGroup>& group)
{
    std::lock_guard<std::mutex> lock(mMutex);
    std::vector<std::shared_ptr<MetalProgramJob>> members;
    for (const std::weak_ptr<MetalProgramJob>& member : group->members)
    {
        if (std::shared_ptr<MetalProgramJob> job = member.lock())
        {
            members.push_back(std::move(job));
        }
    }
    return members;
}

void MetalProgramGroups::translated(MetalProgramJob& job)
{
    std::unique_lock<std::mutex> lock(mMutex);
    --job.group->untranslated;
    submitIfTranslated(lock, job.group);
}

void MetalProgramGroups::cancel()
{
    std::lock_guard<std::mutex> lock(mMutex);
    mCancelled = true;
    if (mOpen)
    {
        closeLocked(mOpen);
    }
}

std::vector<MetalGroupRecord> MetalProgramGroups::records() const
{
    std::lock_guard<std::mutex> lock(mMutex);
    std::vector<MetalGroupRecord> records = mRecords;
    std::sort(records.begin(), records.end(),
              [](const MetalGroupRecord& a, const MetalGroupRecord& b) { return a.number < b.number; });
    return records;
}

U64 MetalProgramGroups::digestSum() const
{
    std::lock_guard<std::mutex> lock(mMutex);
    return mDigestSum;
}

void MetalProgramGroups::submitIfTranslated(std::unique_lock<std::mutex>& lock, const std::shared_ptr<MetalProgramGroup>& group)
{
    if (mCancelled || !group->closed || group->sent || group->untranslated > 0)
    {
        return;
    }
    group->sent = true;
    // A member that failed in translation, or was released before now, is left out.
    std::vector<std::shared_ptr<MetalProgramJob>> members;
    for (const std::weak_ptr<MetalProgramJob>& member : group->members)
    {
        std::shared_ptr<MetalProgramJob> job = member.lock();
        if (job && !job->cancelled.load() && job->phase.load(std::memory_order_acquire) == MetalJobPhase::Translated)
        {
            members.push_back(std::move(job));
        }
    }
    group->members.clear();
    const U64 number = group->number;
    lock.unlock();
    if (members.empty())
    {
        return;
    }
    std::vector<std::array<std::string, 2>> stages;
    for (const std::shared_ptr<MetalProgramJob>& job : members)
    {
        stages.push_back(job->msl);
    }
    const std::string text = metalGroupSource(stages);
    {
        std::lock_guard<std::mutex> recordLock(mMutex);
        const size_t digest = std::hash<std::string>()(text);
        mDigestSum += U64(digest);
        if (mRecords.size() >= MAX_RECORDS)
        {
            mRecords.erase(mRecords.begin()); // the oldest sent
        }
        mRecords.push_back({ number, digest, U32(members.size()) });
    }
    sendGroup(members, text, number);
}

size_t MetalProgramPool::threadCount()
{
    return std::max<size_t>(1, [NSProcessInfo processInfo].activeProcessorCount / 2);
}

void MetalProgramPool::start()
{
    std::lock_guard<std::mutex> lock(mMutex);
    if (!mThreads.empty() || mStopping)
    {
        return;
    }
    for (size_t i = 0; i < threadCount(); ++i)
    {
        mThreads.emplace_back([this]() { run(); });
    }
}

void MetalProgramPool::push(std::shared_ptr<MetalProgramJob> job)
{
    {
        std::lock_guard<std::mutex> lock(mMutex);
        job->seq = ++mSeq;
        mQueue.push_back(std::move(job));
    }
    mChanged.notify_one();
}

bool MetalProgramPool::steal(const MetalProgramJob* job)
{
    std::lock_guard<std::mutex> lock(mMutex);
    const auto queued = std::find_if(mQueue.begin(), mQueue.end(),
                                     [job](const std::shared_ptr<MetalProgramJob>& other) { return other.get() == job; });
    if (queued == mQueue.end())
    {
        return false;
    }
    mQueue.erase(queued);
    return true;
}

std::shared_ptr<MetalProgramJob> MetalProgramPool::takeNext()
{
    std::lock_guard<std::mutex> lock(mMutex);
    if (mQueue.empty())
    {
        return nullptr;
    }
    std::shared_ptr<MetalProgramJob> job = std::move(mQueue.front());
    mQueue.pop_front();
    return job;
}

void MetalProgramPool::pause(bool paused)
{
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mPaused = paused;
    }
    mChanged.notify_all();
}

void MetalProgramPool::stop()
{
    std::deque<std::shared_ptr<MetalProgramJob>> dropped;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mStopping = true;
        dropped.swap(mQueue);
    }
    for (const std::shared_ptr<MetalProgramJob>& job : dropped)
    {
        job->cancelled.store(true);
    }
    mChanged.notify_all();
    for (std::thread& thread : mThreads)
    {
        thread.join();
    }
    mThreads.clear();
}

void MetalProgramPool::run()
{
    pthread_setname_np("Viewer Lab Metal programs");
    pthread_set_qos_class_self_np(qos_class_t(mQos), 0); // user-initiated, utility with the knob
    for (;;)
    {
        std::shared_ptr<MetalProgramJob> job;
        {
            std::unique_lock<std::mutex> lock(mMutex);
            mChanged.wait(lock, [this]() { return mStopping || (!mPaused && !mQueue.empty()); });
            if (mStopping)
            {
                return;
            }
            job = std::move(mQueue.front());
            mQueue.pop_front();
        }
        translateJob(job);
    }
}

/// The program holds its job; Pending until both libraries are made.
class API_AVAILABLE(macos(26.0)) MetalProgram final : public Program
{
public:
    MetalProgram(MetalDevice& device, std::shared_ptr<MetalProgramJob> job) : mDevice(device), mJob(std::move(job)) {}

    ~MetalProgram() override
    {
        // Its pipelines leave the cache, its shared states and refusals the device and its
        // uniforms the encoder. The job is cancelled, and its libraries live
        // until the frames that may use them have finished.
        mDevice.pipelines().forgetProgram(id());
        mDevice.pipelineStates().forgetProgram(id());
        mDevice.metalEncoder().forgetProgram(id());
        release(*mJob);
        if (mPointJob)
        {
            release(*mPointJob); // the point variant goes with its program
        }
    }

    CompileStatus status() const override
    {
        // [Viewer Lab] A creation that a deferred test fault failed answers Pending once, as a program
        // still compiling, and Failed from then on; LLGLSLShader::createShader asks right after createProgram.
        if (mFaultPending.exchange(false, std::memory_order_relaxed))
        {
            mDevice.compileState()->faultPendingAnswers.fetch_add(1, std::memory_order_relaxed);
            return CompileStatus::Pending;
        }
        switch (mJob->phase.load(std::memory_order_acquire))
        {
        case MetalJobPhase::Ready: return CompileStatus::Ready;
        case MetalJobPhase::Failed: return CompileStatus::Failed;
        default: return CompileStatus::Pending;
        }
    }

    void wait() override
    {
        mFaultPending.store(false, std::memory_order_relaxed); // [Viewer Lab] a deferred test fault is Failed now
        // wait() closes the job's group; its members still in the queue are taken from it
        // and translated here, so the group is sent before this waits, from this thread unless a
        // pool thread translates the last member.
        if (mJob->group)
        {
            MetalProgramGroups& groups = mDevice.programGroups();
            groups.close(mJob->group);
            for (const std::shared_ptr<MetalProgramJob>& member : groups.members(mJob->group))
            {
                if (member->phase.load(std::memory_order_acquire) == MetalJobPhase::Queued &&
                    mDevice.programPool().steal(member.get()))
                {
                    if (!member->cancelled.load())
                    {
                        mDevice.countCallerTranslation();
                    }
                    const auto start = std::chrono::steady_clock::now();
                    translateJob(member);
                    addTimeSince(mDevice.compileState()->callerTranslationNs, start);
                }
            }
        }
        // A wait that must wait takes the rest of the pool's queue first, one job at a time from its front
        // beside the pool's threads, until it is empty. Each closed group is sent as soon as its last member is
        // translated, so the groups compile side by side, instead of one after another as each bind in the first world
        // frame waits for its own. The groups close no earlier than without it, so their sources, and Metal's cache
        // keys, stay the same. Not with LL_METAL_COMPILE_SLOW.
        if (!finished(mJob->phase.load(std::memory_order_acquire)) && !mDevice.compileSlow())
        {
            while (std::shared_ptr<MetalProgramJob> next = mDevice.programPool().takeNext())
            {
                if (!next->cancelled.load())
                {
                    mDevice.countCallerTranslation();
                }
                const auto start = std::chrono::steady_clock::now();
                translateJob(next);
                addTimeSince(mDevice.compileState()->callerTranslationNs, start);
            }
        }
        std::unique_lock<std::mutex> lock(mJob->mutex);
        if (!finished(mJob->phase.load(std::memory_order_relaxed)))
        {
            const auto start = std::chrono::steady_clock::now(); // only a wait that waits is timed
            mJob->changed.wait(lock, [this]() { return finished(mJob->phase.load(std::memory_order_relaxed)); });
            countWait(*mDevice.compileState(), MetalWaitKind::Program, start);
        }
        lock.unlock();
        mDevice.writeFailureLines(); // a failed program's line before wait() returns
    }

    /// Complete after wait(); empty while Pending and when Failed. [Viewer Lab] It reads the job's phase, never status(),
    /// so asking for it takes no Pending answer of a deferred test fault.
    const ProgramReflection& reflection() const override
    {
        return mJob->phase.load(std::memory_order_acquire) == MetalJobPhase::Ready ? mJob->reflection : mNone;
    }

    /// [Viewer Lab] createMetalProgram's deferred test fault; the job is Failed already.
    void deferFault() { mFaultPending.store(true, std::memory_order_relaxed); }

    const MetalProgramLayout& layout()
    {
        wait();
        return mJob->layout;
    }

    MetalProgramFunctions functions()
    {
        wait();
        return mJob->functions;
    }

    /// The point variant of a Ready program, made at the first ask on the main thread
    /// and translated alone on the pool (or here, as wait() takes a queued job), then waited
    /// for; empty when it is Ready, otherwise why not. `block` is how its vertex block is made.
    std::string pointVariant(MetalPointBlock& block)
    {
        if (!mPointJob)
        {
            mPointJob = std::make_shared<MetalProgramJob>(mJob);
            mPointJob->translator = mJob->translator;
            mPointJob->compiler = mJob->compiler;
            mPointJob->state = mJob->state;
            mDevice.programPool().start();
            mDevice.programPool().push(mPointJob);
        }
        if (mPointJob->phase.load(std::memory_order_acquire) == MetalJobPhase::Queued &&
            mDevice.programPool().steal(mPointJob.get()))
        {
            mDevice.countCallerTranslation();
            const auto start = std::chrono::steady_clock::now();
            translateJob(mPointJob);
            addTimeSince(mDevice.compileState()->callerTranslationNs, start);
        }
        {
            std::unique_lock<std::mutex> lock(mPointJob->mutex);
            if (!finished(mPointJob->phase.load(std::memory_order_relaxed)))
            {
                const auto start = std::chrono::steady_clock::now();
                mPointJob->changed.wait(lock, [this]()
                                        { return finished(mPointJob->phase.load(std::memory_order_relaxed)); });
                countWait(*mDevice.compileState(), MetalWaitKind::Point, start);
            }
        }
        if (mPointJob->phase.load(std::memory_order_acquire) == MetalJobPhase::Failed)
        {
            mDevice.writeFailureLines(); // the variant's line, as wait() writes a program's
            return "its point variant failed: " + mPointJob->error;
        }
        if (!mPointBlock)
        {
            mPointBlock.emplace();
            mPointWhy = metalPointBlockOf(mJob->reflection, mJob->layout.vertex, mPointJob->reflection,
                                          mPointJob->layout.vertex, *mPointBlock);
        }
        block = *mPointBlock;
        return mPointWhy;
    }

    /// The point variant's vertex function; pointVariant() has made it Ready.
    MTL4FunctionDescriptor* pointVertex() const { return mPointJob ? mPointJob->functions.vertex : nil; }

private:
    /// The job is cancelled, and its libraries live until the frames that may use them have finished.
    void release(MetalProgramJob& job)
    {
        IdMTLLibrary vertex = nil;
        IdMTLLibrary fragment = nil;
        MetalProgramFunctions functions;
        {
            std::lock_guard<std::mutex> lock(job.mutex);
            job.cancelled.store(true);
            std::swap(vertex, job.libraries[0]);
            std::swap(fragment, job.libraries[1]);
            std::swap(functions, job.functions);
        }
        if (vertex || fragment)
        {
            mDevice.lifecycle().release([vertex, fragment, functions]() mutable
            {
                functions = MetalProgramFunctions();
                vertex = nil;
                fragment = nil;
            });
        }
    }

    MetalDevice& mDevice;
    const std::shared_ptr<MetalProgramJob> mJob;
    std::shared_ptr<MetalProgramJob> mPointJob; ///< made at the first point pipeline
    std::optional<MetalPointBlock> mPointBlock;
    std::string mPointWhy;
    const ProgramReflection mNone{};
    /// [Viewer Lab] status() answers Pending once for a deferred test fault; wait() clears it.
    mutable std::atomic<bool> mFaultPending{ false };
};

Ref<Program> createMetalProgram(MetalDevice& device, const ProgramDesc& desc)
{
    // Everything the job reads is copied here, also the names behind the span, and the
    // translator is read here: nothing on another thread reads the ProgramDesc.
    auto job = std::make_shared<MetalProgramJob>(desc);
    job->translator = sTranslator.load();
    MetalProgram* metal = new MetalProgram(device, job);
    Ref<Program> program = metal;
    program->setDebugName(desc.name);
    // [Viewer Lab] Golden level 2's test faults: the shared one first, then Metal's list. A taken
    // creation is Failed before the translator, the compiler and the login screen's branch (Glow Combine is a login
    // program): no WARN line, not in failedPrograms, no group and no pool; deferred, status() answers Pending once.
    bool deferred = false;
    if (takeProgramFault(desc.name, deferred) || takeMetalProgramFault(desc.name, deferred))
    {
        {
            std::lock_guard<std::mutex> lock(job->mutex);
            job->error = "test fault";
            job->phase.store(MetalJobPhase::Failed, std::memory_order_release);
        }
        device.compileState()->faultedPrograms.fetch_add(1, std::memory_order_relaxed);
        if (deferred)
        {
            metal->deferFault();
        }
        return program;
    }
    if (!job->translator)
    {
        failJob(*job, "no shader translator registered (P3-01f)");
        return program;
    }
    job->compiler = device.compiler(); // on the main thread, before the first job
    if (!job->compiler)
    {
        failJob(*job, "the device has no shader compiler");
        return program;
    }
    job->state = device.compileState();
    job->failCompile = device.takeCompileFault();
    job->sleepHandlerMs = device.takeHandlerSleep();
    if (desc.loginScreen)
    {
        // The login screen's programs are translated here and both libraries sent at once;
        // MTL4CompilerTaskOptions has no priority (MTL4Compiler.h:56-77), so the order of sending is.
        device.countCallerTranslation();
        const auto start = std::chrono::steady_clock::now();
        translateJob(job);
        addTimeSince(job->state->callerTranslationNs, start);
        return program;
    }
    if (!metalSafeMath(job->name()) || metalMathKnobs().safeAll)
    {
        device.programGroups().add(job); // before a pool thread can see the job; a Safe-math one alone
    }
    device.programPool().start();
    device.programPool().push(job);
    return program;
}

const MetalProgramLayout* metalProgramLayout(Program* program)
{
    return program ? &static_cast<MetalProgram*>(program)->layout() : nullptr;
}

MetalProgramFunctions metalProgramFunctions(Program* program)
{
    return program ? static_cast<MetalProgram*>(program)->functions() : MetalProgramFunctions();
}

// Pipelines --------------------------------------------------------------------------------------

/// One render pipeline state in the making: Pending until its handler reports, then Ready with
/// the state, which is written before the release-store, or Failed. An unspecialized state keeps
/// the specializations asked for before it was Ready, and its handler sends them.
struct API_AVAILABLE(macos(26.0)) MetalPipelineHolder
{
    std::string name;  ///< the program's, for the WARN line
    PipelineDesc desc; ///< a specialization's attachments, blend and color mask
    std::mutex mutex;
    std::condition_variable changed;
    std::atomic<CompileStatus> status{ CompileStatus::Pending };
    IdMTLRenderPipelineState state = nil;
    std::vector<std::shared_ptr<MetalPipelineHolder>> waiting; ///< under mutex while Pending
    MetalWaitKind kind = MetalWaitKind::Specialized; ///< what a draw waiting for it waits for
};

namespace
{
    // BlendFactor's order (llrhi.h), which is LLRender's, as Metal's factors.
    constexpr MTLBlendFactor BLEND_FACTORS[] = {
        MTLBlendFactorOne,
        MTLBlendFactorZero,
        MTLBlendFactorDestinationColor,
        MTLBlendFactorSourceColor,
        MTLBlendFactorOneMinusDestinationColor,
        MTLBlendFactorOneMinusSourceColor,
        MTLBlendFactorDestinationAlpha,
        MTLBlendFactorSourceAlpha,
        MTLBlendFactorOneMinusDestinationAlpha,
        MTLBlendFactorOneMinusSourceAlpha,
    };
    static_assert(std::size(BLEND_FACTORS) == size_t(BlendFactor::OneMinusSourceAlpha) + 1,
                  "one Metal factor for each BlendFactor");

    // BlendOp's order as Metal's operations; GL's and Metal's subtract are both source minus destination.
    constexpr MTLBlendOperation BLEND_OPS[] = {
        MTLBlendOperationAdd, MTLBlendOperationSubtract, MTLBlendOperationReverseSubtract,
        MTLBlendOperationMin, MTLBlendOperationMax,
    };
    static_assert(std::size(BLEND_OPS) == size_t(BlendOp::Max) + 1, "one Metal operation for each BlendOp");

    MTLBlendFactor blendFactor(BlendFactor factor)
    {
        return BLEND_FACTORS[size_t(factor)];
    }

    MTLBlendOperation blendOp(BlendOp op)
    {
        return BLEND_OPS[size_t(op)];
    }

    std::string metalError(NSError* error)
    {
        return std::string("Metal: ") + (error ? error.localizedDescription.UTF8String : "no error given");
    }

    /// A handler is counted before it is sent; false once the device has stopped waiting.
    bool countHandler(MetalCompileState& state)
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (state.closing)
        {
            return false;
        }
        ++state.outstanding;
        return true;
    }

    /// A handler's last step; late when the device has stopped waiting.
    void handlerDone(MetalCompileState& state)
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.handlersAfterClose += state.closing ? 1 : 0;
        --state.outstanding;
        state.changed.notify_all();
    }

    /// Stores a handler's result and gives the specializations that waited for it. A failure writes
    /// one WARN line before Failed is stored, unless it is quiet: an unspecialized state that failed
    /// wrote the line for its specializations, and nothing is written for a device that is gone.
    std::vector<std::shared_ptr<MetalPipelineHolder>> pipelineDone(MetalPipelineHolder& holder,
                                                                   IdMTLRenderPipelineState made, const std::string& why,
                                                                   bool quiet) API_AVAILABLE(macos(26.0))
    {
        std::lock_guard<std::mutex> lock(holder.mutex);
        if (!made && !quiet)
        {
            LL_WARNS("RHI") << "getPipeline " << holder.name << ": " << why << LL_ENDL;
        }
        holder.state = made;
        holder.status.store(made ? CompileStatus::Ready : CompileStatus::Failed, std::memory_order_release);
        holder.changed.notify_all();
        return std::exchange(holder.waiting, {});
    }

    /// The unspecialized key: the program's attributes, the primitive class and the sample count, no attachments.
    PipelineDesc unspecializedKey(const PipelineDesc& desc, U32 attributes)
    {
        PipelineDesc base;
        base.attributeMask = attributes;
        base.primitiveClass = desc.primitiveClass;
        base.samples = desc.samples;
        base.colorCount = 0;
        return base;
    }

    /// Full pipelines: Metal API Validation warns at run time when a color attachment whose blending
    /// ends Disabled had any unspecialized property in the unspecialized state, even only its format
    /// or write mask (seen in run-time probes; no header says so).
    /// So a description with such an attachment gets a full pipeline: blending off, or an integer
    /// attachment, which GL does not blend (llrhi.h:128-130), with blending on.
    bool endsDisabled(const PipelineDesc& desc)
    {
        if (!desc.blend.enabled)
        {
            return true;
        }
        for (U32 i = 0; i < std::min(desc.colorCount, PipelineDesc::MAX_COLOR_ATTACHMENTS); ++i)
        {
            if (isInteger(desc.colorFormats[i]))
            {
                return true;
            }
        }
        return false;
    }

    /// The blend factors and operations of an attachment (MTL4RenderPipeline.h:40-90).
    void setBlend(MTL4RenderPipelineColorAttachmentDescriptor* attachment, const BlendState& blend)
        API_AVAILABLE(macos(26.0))
    {
        attachment.sourceRGBBlendFactor = blendFactor(blend.srcColor);
        attachment.destinationRGBBlendFactor = blendFactor(blend.dstColor);
        attachment.rgbBlendOperation = blendOp(blend.colorOp);
        attachment.sourceAlphaBlendFactor = blendFactor(blend.srcAlpha);
        attachment.destinationAlphaBlendFactor = blendFactor(blend.dstAlpha);
        attachment.alphaBlendOperation = blendOp(blend.alphaOp);
    }

    /// The program's part of a descriptor (MTL4RenderPipeline.h:156-229): its functions, the vertex
    /// input of its attributes, the sample count (:174) and the topology class (:199). No depth
    /// format: it is not pipeline state in Metal 4. A point pipeline has the point variant's vertex
    /// function and the program's fragment function.
    /// The program's entry points for the description: the point variant's vertex function for points.
    MetalProgramFunctions pipelineFunctions(Program& program, const PipelineDesc& desc) API_AVAILABLE(macos(26.0))
    {
        MetalProgramFunctions functions = metalProgramFunctions(&program);
        if (desc.primitiveClass == PrimitiveClass::Point)
        {
            functions.vertex = static_cast<MetalProgram&>(program).pointVertex();
        }
        return functions;
    }

    /// A descriptor with the functions, the vertex streams, the samples and the topology; name is the label.
    MTL4RenderPipelineDescriptor* functionsDescriptor(const MetalProgramFunctions& functions, const std::string& name,
                                                      const PipelineDesc& desc, U32 attributes)
        API_AVAILABLE(macos(26.0))
    {
        MTL4RenderPipelineDescriptor* descriptor = [MTL4RenderPipelineDescriptor new];
        descriptor.label = metalString(name); // pipelines are always labelled (llrhi.h:456-458)
        descriptor.vertexFunctionDescriptor = functions.vertex;
        descriptor.fragmentFunctionDescriptor = functions.fragment;
        // The streams the program reads; the buffer's other streams stay unbound.
        descriptor.vertexDescriptor = metalVertexDescriptor(attributes);
        descriptor.rasterSampleCount = desc.samples;
        switch (desc.primitiveClass)
        {
        case PrimitiveClass::Point: descriptor.inputPrimitiveTopology = MTLPrimitiveTopologyClassPoint; break;
        case PrimitiveClass::Line:  descriptor.inputPrimitiveTopology = MTLPrimitiveTopologyClassLine; break;
        default:                    descriptor.inputPrimitiveTopology = MTLPrimitiveTopologyClassTriangle; break;
        }
        return descriptor;
    }

    MTL4RenderPipelineDescriptor* programDescriptor(Program& program, const PipelineDesc& desc, U32 attributes)
        API_AVAILABLE(macos(26.0))
    {
        return functionsDescriptor(pipelineFunctions(program, desc), program.debugName(), desc, attributes);
    }

    /// The attachments of a full descriptor. Below colorCount each attachment has its format and write
    /// mask, without alpha for the attachments in alphaOff (bit i for attachment i), and blending
    /// Enabled with the factors when the description blends and the format is not an integer one, else
    /// Disabled; the others have no format. Nothing is unspecialized.
    void setFullAttachments(MTL4RenderPipelineDescriptor* descriptor, const PipelineDesc& desc, U32 alphaOff)
        API_AVAILABLE(macos(26.0))
    {
        const U32 count = std::min(desc.colorCount, PipelineDesc::MAX_COLOR_ATTACHMENTS);
        for (U32 i = 0; i < PipelineDesc::MAX_COLOR_ATTACHMENTS; ++i)
        {
            MTL4RenderPipelineColorAttachmentDescriptor* attachment = descriptor.colorAttachments[i];
            if (i >= count)
            {
                attachment.pixelFormat = MTLPixelFormatInvalid;
                continue;
            }
            attachment.pixelFormat = metalPixelFormat(desc.colorFormats[i]);
            const U8 mask = (alphaOff & (1u << i)) ? U8(desc.colorMask & ~COLOR_MASK_A) : desc.colorMask;
            attachment.writeMask = writeMask(mask); // MTL4RenderPipeline.h:89
            const bool blends = desc.blend.enabled && !isInteger(desc.colorFormats[i]);
            attachment.blendingState = blends ? MTL4BlendStateEnabled : MTL4BlendStateDisabled;
            setBlend(attachment, blends ? desc.blend : BlendState());
        }
    }

    /// (f): the full descriptor of a description with an attachment that ends Disabled. Below
    /// colorCount each attachment has its format and write mask, and blending Enabled with the
    /// factors when the description blends and the format is not an integer one, else Disabled; the
    /// others have no format (a new descriptor's values otherwise: Disabled, One, Zero, Add, all
    /// channels; MTL4RenderPipeline.h:53-83). Nothing is unspecialized.
    MTL4RenderPipelineDescriptor* fullDescriptor(Program& program, const PipelineDesc& desc, U32 attributes)
        API_AVAILABLE(macos(26.0))
    {
        MTL4RenderPipelineDescriptor* descriptor = programDescriptor(program, desc, attributes);
        setFullAttachments(descriptor, desc, 0);
        return descriptor;
    }

    /// The unspecialized descriptor of the descriptions whose attachments all blend:
    /// all eight color attachments (MTL4RenderPipeline.h:196, :40-90) have blending Enabled
    /// (MTL4PipelineState.h:57-61), as GL's blend applies to all draw buffers, and the format
    /// (MTLPixelFormat.h:339), the write mask (MTLRenderPipeline.h:76), the factors (:50) and the
    /// operations (:62) unspecialized. The fragment writes some of them.
    MTL4RenderPipelineDescriptor* unspecializedDescriptor(Program& program, const PipelineDesc& desc, U32 attributes)
        API_AVAILABLE(macos(26.0))
    {
        MTL4RenderPipelineDescriptor* descriptor = programDescriptor(program, desc, attributes);
        for (U32 i = 0; i < PipelineDesc::MAX_COLOR_ATTACHMENTS; ++i)
        {
            MTL4RenderPipelineColorAttachmentDescriptor* attachment = descriptor.colorAttachments[i];
            attachment.pixelFormat = MTLPixelFormatUnspecialized;
            attachment.blendingState = MTL4BlendStateEnabled;
            attachment.writeMask = MTLColorWriteMaskUnspecialized;
            attachment.sourceRGBBlendFactor = MTLBlendFactorUnspecialized;
            attachment.destinationRGBBlendFactor = MTLBlendFactorUnspecialized;
            attachment.rgbBlendOperation = MTLBlendOperationUnspecialized;
            attachment.sourceAlphaBlendFactor = MTLBlendFactorUnspecialized;
            attachment.destinationAlphaBlendFactor = MTLBlendFactorUnspecialized;
            attachment.alphaBlendOperation = MTLBlendOperationUnspecialized;
        }
        return descriptor;
    }

    /// A Ready unspecialized state specialized for the holder's attachments, blend and
    /// mask (MTLRenderPipeline.h:396-412, MTL4Compiler.h:345-348), only for a description whose
    /// attachments all blend: the attachments below colorCount get their format and write mask, the
    /// others no format and all channels; all eight get the blend's factors and operations, with the
    /// blending they had (MTL4Compiler.h:195-205). Counted before it is sent and refused once the
    /// device has stopped waiting. The caller holds no lock: Metal may run the handler on this
    /// thread.
    void sendSpecialization(IdMTL4Compiler compiler, const std::shared_ptr<MetalCompileState>& counter,
                            IdMTLRenderPipelineState unspecialized, const std::shared_ptr<MetalPipelineHolder>& holder)
        API_AVAILABLE(macos(26.0))
    {
        if (!countHandler(*counter))
        {
            pipelineDone(*holder, nil, std::string(), true); // nothing is sent after the device's wait
            return;
        }
        @autoreleasepool // also on Metal's handler threads
        {
            // A descriptor of its own, so every attachment's value is the one set here. Only the
            // unspecialized properties are taken from it (MTL4Compiler.h:197-200).
            MTL4RenderPipelineDescriptor* descriptor = [MTL4RenderPipelineDescriptor new];
            const PipelineDesc& desc = holder->desc;
            const U32 count = std::min(desc.colorCount, PipelineDesc::MAX_COLOR_ATTACHMENTS);
            for (U32 i = 0; i < PipelineDesc::MAX_COLOR_ATTACHMENTS; ++i)
            {
                MTL4RenderPipelineColorAttachmentDescriptor* attachment = descriptor.colorAttachments[i];
                attachment.pixelFormat = i < count ? metalPixelFormat(desc.colorFormats[i]) : MTLPixelFormatInvalid;
                attachment.blendingState = desc.blend.enabled ? MTL4BlendStateEnabled : MTL4BlendStateDisabled;
                setBlend(attachment, desc.blend);
                attachment.writeMask = writeMask(i < count ? desc.colorMask : COLOR_MASK_ALL);
            }
            const std::shared_ptr<MetalCompileState> state = counter;
            __block std::shared_ptr<MetalPipelineHolder> held = holder;
            [compiler newRenderPipelineStateBySpecializationWithDescriptor:descriptor
                                                                  pipeline:unspecialized
                                                         completionHandler:^(id<MTLRenderPipelineState> made, NSError* error)
            {
                pipelineDone(*held, made, made ? std::string() : metalError(error), false);
                held.reset();
                handlerDone(*state);
            }];
        }
    }

    /// An unspecialized state, or a full one, compiled asynchronously
    /// (MTL4Compiler.h:303-305) and counted in MetalCompileState. Its handler sends the
    /// specializations that waited for it (a full state has none) before it counts itself down, so
    /// ~MetalDevice's wait covers them. The caller holds no lock.
    void sendPipeline(IdMTL4Compiler compiler, const std::shared_ptr<MetalCompileState>& counter,
                      MTL4RenderPipelineDescriptor* descriptor, const std::shared_ptr<MetalPipelineHolder>& holder,
                      U32 sleepMs, bool fail) API_AVAILABLE(macos(26.0))
    {
        if (!countHandler(*counter))
        {
            for (const std::shared_ptr<MetalPipelineHolder>& special : pipelineDone(*holder, nil, std::string(), true))
            {
                pipelineDone(*special, nil, std::string(), true);
            }
            return;
        }
        const std::shared_ptr<MetalCompileState> state = counter;
        __block std::shared_ptr<MetalPipelineHolder> held = holder;
        [compiler newRenderPipelineStateWithDescriptor:descriptor
                                   compilerTaskOptions:nil
                                     completionHandler:^(id<MTLRenderPipelineState> made, NSError* error)
        {
            if (sleepMs)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
            }
            // The tests' failNextCompile with no program before it fails the state here.
            id<MTLRenderPipelineState> kept = fail ? nil : made;
            const std::string why = fail ? std::string("the state does not compile (failNextCompile)")
                                         : kept ? std::string() : metalError(error);
            for (const std::shared_ptr<MetalPipelineHolder>& special : pipelineDone(*held, kept, why, false))
            {
                if (kept)
                {
                    sendSpecialization(compiler, state, kept, special);
                }
                else
                {
                    pipelineDone(*special, nil, std::string(), true);
                }
            }
            held.reset();
            handlerDone(*state);
        }];
    }
}

std::shared_ptr<MetalPipelineHolder> MetalPipelineStates::state(MetalDevice& device, Program& program,
                                                                const PipelineDesc& desc, U32 attributes)
{
    // The keys: none has the depth format, and the unspecialized one has no attachments. A key
    // with blending off has no factors (normalized, llrhi.cpp).
    PipelineDesc special = normalized(desc);
    special.attributeMask = attributes;
    special.depthFormat = Format::Unknown;
    // A device made for a frame capture has full states only (MetalDevice::fullPipelineStates); the full
    // descriptor blends as the specialization would. So has a program with program-scope constant data, whose
    // reads shader validation reports in a specialized state (MetalProgramLayout::constantData).
    if (device.fullPipelineStates() || endsDisabled(special) || metalProgramLayout(&program)->constantData)
    {
        // (f): a full state of its own, shared across depth formats and unread attributes.
        std::shared_ptr<MetalPipelineHolder> full;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            std::shared_ptr<MetalPipelineHolder>& found = mFull[PipelineKey{ program.id(), special }];
            if (found)
            {
                return found;
            }
            found = std::make_shared<MetalPipelineHolder>();
            found->name = program.debugName();
            found->desc = special;
            found->kind = MetalWaitKind::Full;
            full = found;
            ++mSent.full;
            mBases.insert(PipelineKey{ program.id(), unspecializedKey(desc, attributes) }); // the would-be unspecialized one
        }
        MTL4RenderPipelineDescriptor* descriptor = fullDescriptor(program, special, attributes);
        // For tests: what is sent, read back from the descriptor, so a full state that blends with
        // One, Zero and Add instead of Disabled is seen.
        U32 disabled = 0;
        for (U32 i = 0; i < std::min(special.colorCount, PipelineDesc::MAX_COLOR_ATTACHMENTS); ++i)
        {
            if (descriptor.colorAttachments[i].blendingState == MTL4BlendStateDisabled)
            {
                disabled |= 1u << i;
            }
        }
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mLastFullDisabled = disabled;
        }
        sendPipeline(device.compiler(), device.compileState(), descriptor, full, device.takeHandlerSleep(),
                     device.takeCompileFault());
        return full;
    }
    const PipelineDesc base = unspecializedKey(desc, attributes);

    std::shared_ptr<MetalPipelineHolder> specialized;
    std::shared_ptr<MetalPipelineHolder> made; // a new unspecialized state, sent below
    IdMTLRenderPipelineState ready = nil;      // a Ready unspecialized state, specialized below
    {
        std::lock_guard<std::mutex> lock(mMutex);
        auto found = mSpecialized.find({ program.id(), special });
        if (found != mSpecialized.end())
        {
            return found->second;
        }
        specialized = std::make_shared<MetalPipelineHolder>();
        specialized->name = program.debugName();
        specialized->desc = special;
        mSpecialized.emplace(PipelineKey{ program.id(), special }, specialized);
        ++mSent.specialized;
        std::shared_ptr<MetalPipelineHolder>& unspecialized = mUnspecialized[PipelineKey{ program.id(), base }];
        if (!unspecialized)
        {
            unspecialized = std::make_shared<MetalPipelineHolder>();
            unspecialized->name = program.debugName();
            made = unspecialized;
            ++mSent.unspecialized;
            mBases.insert(PipelineKey{ program.id(), base });
        }
        // The holder's lock after this one; the handlers take the holder's alone.
        std::lock_guard<std::mutex> holderLock(unspecialized->mutex);
        switch (unspecialized->status.load(std::memory_order_acquire))
        {
        case CompileStatus::Pending:
            unspecialized->waiting.push_back(specialized); // its handler sends it
            break;
        case CompileStatus::Ready:
            ready = unspecialized->state;
            break;
        case CompileStatus::Failed:
            specialized->status.store(CompileStatus::Failed, std::memory_order_release); // its WARN line is written
            break;
        }
    }
    if (made)
    {
        sendPipeline(device.compiler(), device.compileState(), unspecializedDescriptor(program, desc, attributes), made,
                     device.takeHandlerSleep(),
                     device.takeCompileFault());
    }
    else if (ready)
    {
        sendSpecialization(device.compiler(), device.compileState(), ready, specialized);
    }
    return specialized;
}

bool MetalPipelineStates::refuse(const PipelineKey& key)
{
    std::lock_guard<std::mutex> lock(mMutex);
    const bool first = mRefused.insert(key).second;
    mSent.refused += first ? 1 : 0;
    return first;
}

void MetalPipelineStates::countVariant()
{
    std::lock_guard<std::mutex> lock(mMutex);
    ++mSent.alphaVariants;
}

MetalCompileStats MetalPipelineStates::stats() const
{
    std::lock_guard<std::mutex> lock(mMutex);
    MetalCompileStats stats = mSent;
    stats.e14Unspecialized = mBases.size() + mForgottenBases;
    return stats;
}

size_t MetalPipelineStates::heldBases() const
{
    std::lock_guard<std::mutex> lock(mMutex);
    return mBases.size();
}

void MetalPipelineStates::forgetProgram(U64 programId)
{
    const auto ofProgram = [programId](const auto& entry) { return entry.first.programId == programId; };
    std::lock_guard<std::mutex> lock(mMutex);
    std::erase_if(mUnspecialized, ofProgram);
    std::erase_if(mSpecialized, ofProgram);
    std::erase_if(mFull, ofProgram);
    std::erase_if(mRefused, [programId](const PipelineKey& key) { return key.programId == programId; });
    // The program's unspecialized base keys are only counted from now on
    mForgottenBases += std::erase_if(mBases, [programId](const PipelineKey& key) { return key.programId == programId; });
}

void MetalPipelineStates::clear()
{
    std::lock_guard<std::mutex> lock(mMutex);
    mUnspecialized.clear();
    mSpecialized.clear();
    mFull.clear();
    mRefused.clear();
    mLastFullDisabled = 0;
}

size_t MetalPipelineStates::unspecialized() const
{
    std::lock_guard<std::mutex> lock(mMutex);
    return mUnspecialized.size();
}

size_t MetalPipelineStates::specialized() const
{
    std::lock_guard<std::mutex> lock(mMutex);
    return mSpecialized.size();
}

size_t MetalPipelineStates::full() const
{
    std::lock_guard<std::mutex> lock(mMutex);
    return mFull.size();
}

size_t MetalPipelineStates::refused() const
{
    std::lock_guard<std::mutex> lock(mMutex);
    return mRefused.size();
}

U32 MetalPipelineStates::lastFullDisabled() const
{
    std::lock_guard<std::mutex> lock(mMutex);
    return mLastFullDisabled;
}

/// One pipeline per full key in PipelineCache, so the tracker compares its depth format
/// with the pass's (llrhi.cpp:1026). Its state is the shared specialized or full one, which it holds
/// until the frames that may have used it have finished.
class API_AVAILABLE(macos(26.0)) MetalPipeline final : public Pipeline
{
public:
    /// layout and attributes: the program's, so a draw needs only the bound pipeline; the
    /// tracker can hold the pipeline after the program has gone.
    /// functions: the program's for the description, so a variant is made without the program.
    /// compile: the device's, for the time a draw waits for the state.
    MetalPipeline(FrameLifecycle& lifecycle, U64 programId, const PipelineDesc& desc,
                  std::shared_ptr<MetalPipelineHolder> holder, const MetalProgramLayout& layout, U32 attributes,
                  const MetalPointBlock& point, const MetalProgramFunctions& functions,
                  std::shared_ptr<MetalCompileState> compile)
    :   mLifecycle(lifecycle), mProgramId(programId), mDesc(desc), mHolder(std::move(holder)), mLayout(layout),
        mAttributes(attributes), mPoint(point), mFunctions(functions), mCompile(std::move(compile))
    {
    }

    ~MetalPipeline() override
    {
        std::shared_ptr<MetalPipelineHolder> holder = std::move(mHolder);
        std::unordered_map<U32, std::shared_ptr<MetalPipelineHolder>> variants = std::move(mVariants);
        mLifecycle.release([holder, variants]() mutable
        {
            holder.reset();
            variants.clear();
        });
    }

    U64 programId() const override { return mProgramId; }
    const PipelineDesc& desc() const override { return mDesc; }
    CompileStatus status() const override { return mHolder->status.load(std::memory_order_acquire); }
    /// Waits while the state is being made (Pipeline's contract, llrhi.h:790-791); nil when it failed.
    IdMTLRenderPipelineState state() const
    {
        std::unique_lock<std::mutex> lock(mHolder->mutex);
        if (mHolder->status.load(std::memory_order_relaxed) == CompileStatus::Pending)
        {
            const auto start = std::chrono::steady_clock::now(); // only a wait that waits is timed
            mHolder->changed.wait(lock, [this]()
                                  { return mHolder->status.load(std::memory_order_relaxed) != CompileStatus::Pending; });
            countWait(*mCompile, mHolder->kind, start);
        }
        return mHolder->state;
    }
    const MetalProgramLayout& layout() const { return mLayout; }
    U32 attributes() const { return mAttributes; }
    /// The point variant's vertex block; null for another class.
    const MetalPointBlock* point() const { return mDesc.primitiveClass == PrimitiveClass::Point ? &mPoint : nullptr; }
    /// The state for a pass whose attachments in alphaOff (bit i for attachment i) have no alpha in
    /// GL: GL keeps their alpha 1, so the variant's write mask has no alpha there (MTL4RenderPipeline.h:89). The
    /// key is this pipeline and the mask; the variant is a full state of its own, made at the first ask and
    /// waited for as any state; nil when it or this pipeline's state failed. Main thread, as the draws.
    IdMTLRenderPipelineState state(MetalDevice& device, U32 alphaOff)
    {
        alphaOff &= (1u << std::min(mDesc.colorCount, PipelineDesc::MAX_COLOR_ATTACHMENTS)) - 1;
        IdMTLRenderPipelineState base = state();
        if (!base || alphaOff == 0 || !(mDesc.colorMask & COLOR_MASK_A))
        {
            return base; // nothing to take out
        }
        std::shared_ptr<MetalPipelineHolder>& variant = mVariants[alphaOff];
        if (!variant)
        {
            variant = std::make_shared<MetalPipelineHolder>();
            variant->name = mHolder->name;
            variant->kind = MetalWaitKind::Variant;
            PipelineDesc special = normalized(mDesc);
            special.attributeMask = mAttributes;
            special.depthFormat = Format::Unknown;
            variant->desc = special;
            MTL4RenderPipelineDescriptor* descriptor =
                functionsDescriptor(mFunctions, mHolder->name, special, mAttributes);
            setFullAttachments(descriptor, special, alphaOff);
            sendPipeline(device.compiler(), device.compileState(), descriptor, variant, device.takeHandlerSleep(),
                         device.takeCompileFault());
            device.pipelineStates().countVariant();
        }
        std::unique_lock<std::mutex> lock(variant->mutex);
        if (variant->status.load(std::memory_order_relaxed) == CompileStatus::Pending)
        {
            const auto start = std::chrono::steady_clock::now();
            variant->changed.wait(lock, [&variant]()
                                  { return variant->status.load(std::memory_order_relaxed) != CompileStatus::Pending; });
            countWait(*mCompile, MetalWaitKind::Variant, start);
        }
        return variant->state;
    }
    /// Tests: the variants made so far.
    size_t variants() const { return mVariants.size(); }

private:
    FrameLifecycle& mLifecycle;
    const U64 mProgramId;
    const PipelineDesc mDesc;
    std::shared_ptr<MetalPipelineHolder> mHolder;
    const MetalProgramLayout mLayout;
    const U32 mAttributes;
    const MetalPointBlock mPoint;
    const MetalProgramFunctions mFunctions;
    const std::shared_ptr<MetalCompileState> mCompile;
    std::unordered_map<U32, std::shared_ptr<MetalPipelineHolder>> mVariants; ///< by alphaOff
};

Ref<Pipeline> createMetalPipeline(MetalDevice& device, Program& program, const PipelineDesc& desc)
{
    // The streams the program reads: attributes outside its mask change nothing.
    const U32 attributes = desc.attributeMask & program.reflection().attributeMask;
    MetalPipelineStates& states = device.pipelineStates();
    const auto refuse = [&](const std::string& why) -> Ref<Pipeline>
    {
        PipelineDesc key = desc;
        key.attributeMask = attributes;
        key.depthFormat = Format::Unknown;
        if (states.refuse({ program.id(), key }))
        {
            LL_WARNS("RHI") << "getPipeline " << program.debugName() << ": " << why << LL_ENDL;
        }
        return nullptr;
    };
    if (program.status() != CompileStatus::Ready)
    {
        return refuse("the program failed to compile");
    }
    MetalPointBlock point;
    if (desc.primitiveClass == PrimitiveClass::Point)
    {
        // The point variant, made at the first point pipeline and waited for.
        const std::string why = static_cast<MetalProgram&>(program).pointVariant(point);
        if (!why.empty())
        {
            return refuse(why);
        }
    }
    if (desc.samples == 0 || ![device.mtlDevice() supportsTextureSampleCount:desc.samples])
    {
        return refuse("the device has no " + std::to_string(desc.samples) + "-sample rendering");
    }
    for (U32 i = 0; i < std::min(desc.colorCount, PipelineDesc::MAX_COLOR_ATTACHMENTS); ++i)
    {
        if (metalPixelFormat(desc.colorFormats[i]) == MTLPixelFormatInvalid)
        {
            return refuse("color attachment " + std::to_string(i) + " has no Metal format");
        }
    }
    Ref<Pipeline> pipeline =
        new MetalPipeline(device.lifecycle(), program.id(), desc, states.state(device, program, desc, attributes),
                          *metalProgramLayout(&program), attributes, point, pipelineFunctions(program, desc),
                          device.compileState());
    pipeline->setDebugName(program.debugName());
    return pipeline;
}

// A native pipeline (Pipeline::native) is not a MetalPipeline: it has its state and nothing else of these.

IdMTLRenderPipelineState metalPipelineState(Pipeline* pipeline)
{
    if (pipeline && pipeline->native())
    {
        return metalNativePipelineState(pipeline);
    }
    return pipeline ? static_cast<MetalPipeline*>(pipeline)->state() : nil;
}

IdMTLRenderPipelineState metalPipelineState(Pipeline* pipeline, MetalDevice& device, U32 alphaOff)
{
    if (pipeline && pipeline->native())
    {
        return metalNativePipelineState(pipeline); // its program writes the alpha it means to
    }
    return pipeline ? static_cast<MetalPipeline*>(pipeline)->state(device, alphaOff) : nil;
}

size_t metalPipelineVariants(Pipeline* pipeline)
{
    return pipeline && !pipeline->native() ? static_cast<MetalPipeline*>(pipeline)->variants() : 0;
}

const MetalProgramLayout* metalPipelineLayout(Pipeline* pipeline)
{
    return pipeline && !pipeline->native() ? &static_cast<MetalPipeline*>(pipeline)->layout() : nullptr;
}

U32 metalPipelineAttributes(Pipeline* pipeline)
{
    return pipeline && !pipeline->native() ? static_cast<MetalPipeline*>(pipeline)->attributes() : 0;
}

const MetalPointBlock* metalPipelinePointBlock(Pipeline* pipeline)
{
    return pipeline && !pipeline->native() ? static_cast<MetalPipeline*>(pipeline)->point() : nullptr;
}

void metalSetBlend(MTL4RenderPipelineColorAttachmentDescriptor* attachment, const BlendState& blend)
{
    setBlend(attachment, blend);
}

MTLColorWriteMask metalWriteMask(U8 mask)
{
    return writeMask(mask);
}

std::string metalPointBlockOf(const ProgramReflection& program, const MetalStageLayout& programVertex,
                              const ProgramReflection& variant, const MetalStageLayout& variantVertex,
                              MetalPointBlock& block)
{
    block = MetalPointBlock();
    block.size = variantVertex.blockSize;
    block.same = true;
    const auto variantPlace = [&](const std::string& name) -> const MetalUniformPlace*
    {
        for (size_t v = 0; v < variant.uniforms.size() && v < variantVertex.places.size(); ++v)
        {
            if (variant.uniforms[v].name == name && variantVertex.places[v].size != 0)
            {
                return &variantVertex.places[v];
            }
        }
        return nullptr;
    };
    for (size_t u = 0; u < program.uniforms.size() && u < programVertex.places.size(); ++u)
    {
        const MetalUniformPlace& from = programVertex.places[u];
        if (from.size == 0)
        {
            continue; // the vertex stage does not read it
        }
        const MetalUniformPlace* to = variantPlace(program.uniforms[u].name);
        if (!to || to->size != from.size)
        {
            return "the point variant's vertex stage has no uniform " + program.uniforms[u].name + " of " +
                   std::to_string(from.size) + " bytes";
        }
        block.copies.push_back({ from.offset, to->offset, from.size });
        block.same = block.same && from.offset == to->offset;
    }
    const MetalUniformPlace* pointSize = variantPlace("point_size");
    if (!pointSize || pointSize->size < sizeof(F32))
    {
        return "the point variant's vertex stage has no point_size";
    }
    block.pointSize = pointSize->offset;
    if (block.same)
    {
        block.copies.clear(); // one memcpy of the program's block
    }
    return {};
}

void metalBuildPointBlock(const MetalPointBlock& block, std::span<const std::byte> program, F32 pointSize,
                          std::vector<std::byte>& out)
{
    out.assign(block.size, std::byte(0));
    if (block.same && !program.empty() && !out.empty())
    {
        std::memcpy(out.data(), program.data(), std::min<size_t>(program.size(), out.size()));
    }
    for (const MetalPointBlock::Copy& copy : block.copies)
    {
        if (size_t(copy.from) + copy.size <= program.size() && size_t(copy.to) + copy.size <= out.size())
        {
            std::memcpy(out.data() + copy.to, program.data() + copy.from, copy.size);
        }
    }
    if (size_t(block.pointSize) + sizeof(pointSize) <= out.size())
    {
        std::memcpy(out.data() + block.pointSize, &pointSize, sizeof(pointSize));
    }
}

} // namespace LL::RHI

#endif // LL_RHI_METAL
