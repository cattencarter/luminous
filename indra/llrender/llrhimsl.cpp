/**
 * @file llrhimsl.cpp
 * @brief Translation of the viewer's GLSL programs to MSL for the Metal backend
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

#include "linden_common.h"

#include "llrhimsl.h"

#include "hbxxh.h"
#include "llfile.h"
#include "llsdserialize.h"
#include "llsdutil.h"

#include "glslang/Public/ResourceLimits.h"
#include "glslang/Public/ShaderLang.h"
#include "SPIRV/GlslangToSpv.h"
#include "spirv_msl.hpp"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <unistd.h>

namespace
{
    using namespace LL::RHI::MSL;

    /// glslang's messages: SPIR-V and Vulkan rules, relaxed through the shader's environment.
    const EShMessages MESSAGES = EShMessages(EShMsgSpvRules | EShMsgVulkanRules);

    /// The name glslang gives the block it gathers a stage's loose uniforms into; buffer 0 of the
    /// stage.
    const char* const DEFAULT_BLOCK = "DefaultUniforms";

    /// The GLSL version glslang reads the units as. The GL viewer writes #version 400 on macOS;
    /// 4.20 is the first that allows a line continuation, which Apple's GL accepts at 4.00 too (in
    /// SMAA.glsl's banner comment), and changes nothing else the viewer's shaders use.
    const int GLSL_VERSION = 420;

    /// In every cache key: bump it with any change to what this file produces.
    const int TRANSLATION_VERSION = 4;

    /// The vendored SPIRV-Cross release (indra/third_party/README.md), in every cache key.
    const char* const SPIRV_CROSS_RELEASE = "vulkan-sdk-1.4.363.0";

    /// Buffers 0-7 hold uniforms.
    const U32 MAX_UNIFORM_BUFFER = 7;

    /// glslang warnings that do not change what the program means. Every other warning is an
    /// error.
    const char* const ALLOWED_WARNINGS[] = {
        "names containing consecutive underscores are reserved", // FXAA_QUALITY__PRESET, a define
    };

    EShLanguage language(Stage stage)
    {
        switch (stage)
        {
        case Stage::Vertex:
            return EShLangVertex;
        case Stage::Fragment:
            return EShLangFragment;
        case Stage::Geometry:
            return EShLangGeometry;
        }
        return EShLangVertex;
    }

    void initialize()
    {
        static std::once_flag once;
        std::call_once(once, [] { glslang::InitializeProcess(); });
    }

    /// The lines of a glslang log that are warnings, with the unit's file in front.
    void collectWarnings(const std::string& log, const std::string& file, std::vector<std::string>& warnings,
                         std::vector<std::string>& errors)
    {
        std::istringstream lines(log);
        std::string line;
        while (std::getline(lines, line))
        {
            if (line.rfind("WARNING:", 0) != 0)
            {
                continue;
            }
            const bool allowed = std::any_of(std::begin(ALLOWED_WARNINGS), std::end(ALLOWED_WARNINGS),
                                             [&line](const char* text) { return *text && line.find(text) != std::string::npos; });
            (allowed ? warnings : errors).push_back(file + ": " + line);
        }
    }

    /// One stage's units linked as one glslang program, as SPIR-V; empty with `error` set when it
    /// fails.
    const char* const POINT_SIZE_UNIT = "llrhimsl/pointsizeV.glsl";

    std::vector<U32> linkStage(const Program& program, Stage stage, bool point_size, std::vector<std::string>& warnings,
                               std::string& error)
    {
        const EShLanguage lang = language(stage);
        std::vector<std::unique_ptr<glslang::TShader>> shaders;
        glslang::TProgram linked;
        std::vector<std::string> errors;
        for (const Unit& unit : program.units)
        {
            if (unit.stage != stage)
            {
                continue;
            }
            auto shader = std::make_unique<glslang::TShader>(lang);
            const char* source = unit.source.c_str();
            const char* name = unit.file.c_str();
            shader->setStringsWithLengthsAndNames(&source, nullptr, &name, 1);
            if (point_size && stage == Stage::Vertex && unit.file != POINT_SIZE_UNIT)
            {
                // The point variant: the program's main becomes a function the point unit calls.
                shader->setPreamble("#define main ll_point_main\n");
            }
            shader->setOverrideVersion(GLSL_VERSION);
            shader->setEnvInput(glslang::EShSourceGlsl, lang, glslang::EShClientVulkan, 100);
            shader->setEnvInputVulkanRulesRelaxed();
            shader->setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_1);
            shader->setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_3);
            shader->setAutoMapLocations(true);
            shader->setAutoMapBindings(true);
            shader->setGlobalUniformBlockName(DEFAULT_BLOCK);
            shader->setGlobalUniformSet(0);
            shader->setGlobalUniformBinding(0);
            if (!shader->parse(GetDefaultResources(), 100, false, MESSAGES))
            {
                error = unit.file + ": " + shader->getInfoLog();
                return {};
            }
            collectWarnings(shader->getInfoLog(), unit.file, warnings, errors);
            linked.addShader(shader.get());
            shaders.push_back(std::move(shader));
        }
        if (shaders.empty())
        {
            error = std::string("no ") + stageName(stage) + " stage";
            return {};
        }
        if (!linked.link(MESSAGES) || !linked.mapIO())
        {
            error = std::string(stageName(stage)) + " stage: " + linked.getInfoLog();
            return {};
        }
        collectWarnings(linked.getInfoLog(), std::string(stageName(stage)) + " stage", warnings, errors);
        if (!errors.empty())
        {
            error = "glslang warnings are errors (R1-04): " + errors.front()
                  + (errors.size() > 1 ? llformat(" (and %d more)", (S32)errors.size() - 1) : std::string());
            return {};
        }
        std::vector<U32> spirv;
        spv::SpvBuildLogger logger;
        glslang::GlslangToSpv(*linked.getIntermediate(lang), spirv, &logger);
        if (!logger.getAllMessages().empty())
        {
            // GlslangToSpv's warnings and missing functionality are errors too.
            error = std::string(stageName(stage)) + " stage to SPIR-V: " + logger.getAllMessages();
            return {};
        }
        return spirv;
    }

    std::string typeName(const spirv_cross::SPIRType& type)
    {
        std::string name;
        switch (type.basetype)
        {
        case spirv_cross::SPIRType::Float:
            name = "float";
            break;
        case spirv_cross::SPIRType::Int:
            name = "int";
            break;
        case spirv_cross::SPIRType::UInt:
            name = "uint";
            break;
        case spirv_cross::SPIRType::Boolean:
            name = "bool";
            break;
        default:
            name = llformat("type%d", (S32)type.basetype);
            break;
        }
        if (type.columns > 1)
        {
            name += llformat("%ux%u", type.columns, type.vecsize);
        }
        else if (type.vecsize > 1)
        {
            name += llformat("%u", type.vecsize);
        }
        for (U32 size : type.array)
        {
            name += llformat("[%u]", size);
        }
        return name;
    }

    Variable variable(spirv_cross::Compiler& compiler, const spirv_cross::Resource& resource)
    {
        Variable result;
        result.name = compiler.get_name(resource.id);
        result.type = typeName(compiler.get_type(resource.type_id));
        result.location = compiler.get_decoration(resource.id, spv::DecorationLocation);
        result.flat = compiler.has_decoration(resource.id, spv::DecorationFlat);
        return result;
    }

    std::string textureType(const spirv_cross::SPIRType& type)
    {
        std::string name;
        switch (type.image.dim)
        {
        case spv::Dim2D:
            name = type.image.arrayed ? "2d_array" : "2d";
            break;
        case spv::DimCube:
            name = type.image.arrayed ? "cube_array" : "cube";
            break;
        case spv::Dim3D:
            name = "3d";
            break;
        case spv::Dim1D:
            name = "1d";
            break;
        default:
            name = llformat("dim%d", (S32)type.image.dim);
            break;
        }
        if (type.image.dim == spv::DimBuffer)
        {
            name = "buffer";
        }
        if (type.image.ms)
        {
            name += "_ms";
        }
        return type.image.depth ? name + "_shadow" : name;
    }

    /// Fixed MSL indices for a stage's resources. False with why for a block GL
    /// does not bind.
    bool bindResources(spirv_cross::CompilerMSL& compiler, const Program& program, std::string& why)
    {
        const spv::ExecutionModel model = compiler.get_execution_model();
        // Only what the stage uses gets an index, as GL counts only active samplers.
        const spirv_cross::ShaderResources resources = compiler.get_shader_resources(compiler.get_active_interface_variables());
        for (const spirv_cross::Resource& block : resources.uniform_buffers)
        {
            const std::string name = compiler.get_name(block.base_type_id);
            U32 buffer = 0;
            if (name != DEFAULT_BLOCK)
            {
                const auto found = std::find(program.blocks.begin(), program.blocks.end(), name);
                if (found == program.blocks.end())
                {
                    why = "uniform block " + name + " has no GL binding";
                    return false;
                }
                buffer = 1 + U32(found - program.blocks.begin());
                if (buffer > MAX_UNIFORM_BUFFER)
                {
                    why = llformat("uniform block %s would be buffer %u; decision 4 has 0-%u for uniforms", name.c_str(), buffer,
                                   MAX_UNIFORM_BUFFER);
                    return false;
                }
            }
            spirv_cross::MSLResourceBinding binding;
            binding.stage = model;
            binding.desc_set = compiler.get_decoration(block.id, spv::DecorationDescriptorSet);
            binding.binding = compiler.get_decoration(block.id, spv::DecorationBinding);
            binding.msl_buffer = buffer;
            compiler.add_msl_resource_binding(binding);
        }
        std::vector<const spirv_cross::Resource*> samplers;
        for (const spirv_cross::Resource& sampler : resources.sampled_images)
        {
            samplers.push_back(&sampler);
        }
        std::sort(samplers.begin(), samplers.end(), [&compiler](const auto* a, const auto* b)
                  { return compiler.get_name(a->id) < compiler.get_name(b->id); });
        for (U32 i = 0; i < samplers.size(); ++i)
        {
            spirv_cross::MSLResourceBinding binding;
            binding.stage = model;
            binding.desc_set = compiler.get_decoration(samplers[i]->id, spv::DecorationDescriptorSet);
            binding.binding = compiler.get_decoration(samplers[i]->id, spv::DecorationBinding);
            binding.msl_texture = i;
            binding.msl_sampler = i;
            compiler.add_msl_resource_binding(binding);
        }
        return true;
    }

    /// SPIRV-Cross's MSL compiler with the active sizes of the default block's arrays.
    /// The walk is get_active_buffer_ranges' (spirv_cross.cpp:2352-2404) with the indices kept; its
    /// OpcodeHandler and traversal are protected (spirv_cross.hpp:826-845, 963-964), hence the subclass.
    class Translator : public spirv_cross::CompilerMSL
    {
    public:
        using spirv_cross::CompilerMSL::CompilerMSL;

        /// For the array members of `type`, the block of variable `block`, by member index: the
        /// highest constant index the entry point's call tree reads + 1, or DECLARED for an index
        /// that is not a constant or is a specialization constant, an array of arrays or a use of
        /// the whole array. A member it does not read has no entry.
        static constexpr U32 DECLARED = U32(~0u);
        std::map<U32, U32> activeArraySizes(U32 block, const spirv_cross::SPIRType& type)
        {
            ArrayHandler handler(*this, block, type);
            traverse_all_reachable_opcodes(get<spirv_cross::SPIRFunction>(ir.default_entry_point), handler);
            return handler.sizes;
        }

        /// Before compile(), the position output Invariant, a block member's or a plain variable's;
        /// update_active_builtins then sets position_invariant, which gives [[position, invariant]].
        void makePositionInvariant()
        {
            const spirv_cross::ShaderResources resources = get_shader_resources();
            for (const spirv_cross::BuiltInResource& output : resources.builtin_outputs)
            {
                if (output.builtin != spv::BuiltInPosition)
                {
                    continue;
                }
                const spirv_cross::SPIRType& type = get_type(output.resource.base_type_id);
                if (type.basetype != spirv_cross::SPIRType::Struct)
                {
                    set_decoration(output.resource.id, spv::DecorationInvariant);
                    continue;
                }
                for (U32 member = 0; member < U32(type.member_types.size()); ++member)
                {
                    if (has_member_decoration(type.self, member, spv::DecorationBuiltIn) &&
                        get_member_decoration(type.self, member, spv::DecorationBuiltIn) == spv::BuiltInPosition)
                    {
                        set_member_decoration(type.self, member, spv::DecorationInvariant);
                    }
                }
            }
        }

        /// Before compile(), when the stage samples a depth image (glslang's sampler2DShadow), the
        /// identity spvDepthCast overloads for the depth types image_type_glsl declares it as. SPIRV-Cross wraps
        /// every Dref sampling in spvDepthCast (spirv_msl.cpp:12505-12512), whose own overloads take the color
        /// types; the header lines come before "using namespace metal;" (spirv_msl.cpp:5855-5860).
        void declareDepthImages()
        {
            for (const spirv_cross::Resource& sampler : get_shader_resources().sampled_images)
            {
                if (get_type(sampler.type_id).image.depth)
                {
                    for (const auto& [color, depth] : DEPTH_TYPES)
                    {
                        add_header_line(std::string("template <typename T> static inline metal::") + depth +
                                        "<T> spvDepthCast(metal::" + depth + "<T> t) { return t; }");
                    }
                    return;
                }
            }
        }

    protected:
        /// A depth image as Metal's depth texture type, on which MSL's sample_compare is defined, in the
        /// entry point and in every function that takes it. SPIRV-Cross declares it as a color texture and
        /// reinterpret_casts that at each Dref sampling; under Metal's shader validation the cast texture reads as
        /// a resource usage mismatch and gives 0 for every comparison (seen in a run-time probe); without
        /// validation it compares. glslang marks sampler2DShadow's image as depth, as the reflection's _shadow
        /// names rely on (textureType).
        std::string image_type_glsl(const spirv_cross::SPIRType& type, uint32_t id, bool member) override
        {
            std::string name = spirv_cross::CompilerMSL::image_type_glsl(type, id, member);
            if (type.image.depth != 1 || type.image.ms || type.image.sampled == 2 || !type.array.empty())
            {
                return name; // an array's element type came through this override already
            }
            for (const auto& [color, depth] : DEPTH_TYPES)
            {
                const std::string prefix = std::string(color) + "<";
                if (name.compare(0, prefix.size(), prefix) == 0)
                {
                    return depth + name.substr(prefix.size() - 1);
                }
            }
            return name;
        }

    private:
        /// SPIRV-Cross's color texture types with an spvDepthCast (spirv_msl.cpp:8319-8345) and their depth types.
        static constexpr std::pair<const char*, const char*> DEPTH_TYPES[] = {
            { "texture2d", "depth2d" },
            { "texture2d_array", "depth2d_array" },
            { "texturecube", "depthcube" },
            { "texturecube_array", "depthcube_array" },
        };

        struct ArrayHandler : OpcodeHandler
        {
            ArrayHandler(Translator& translator, U32 block, const spirv_cross::SPIRType& type)
                : OpcodeHandler(translator), self(translator), block(block), type(type)
            {
            }

            // Access chain: args are its type, result, base and indices.
            bool handle(spv::Op opcode, const uint32_t* args, uint32_t length) override
            {
                const bool chain = opcode == spv::OpAccessChain || opcode == spv::OpInBoundsAccessChain;
                if (chain && length >= 4 && args[2] == block)
                {
                    const spirv_cross::SPIRConstant* member = self.maybe_get<spirv_cross::SPIRConstant>(args[3]);
                    if (member && member->scalar() < type.member_types.size())
                    {
                        if (length >= 5)
                        {
                            count(member->scalar(), args[4]);
                        }
                        else
                        {
                            members[args[1]] = member->scalar(); // the chain stops at the member
                        }
                        return true;
                    }
                }
                if (chain && length >= 4)
                {
                    const auto found = members.find(args[2]);
                    if (found != members.end())
                    {
                        count(found->second, args[3]);
                        return true;
                    }
                }
                // Any other use of a member's pointer, or of the block's, uses all of it.
                for (uint32_t i = 0; i < length; ++i)
                {
                    const auto found = members.find(args[i]);
                    if (found != members.end())
                    {
                        sizes[found->second] = DECLARED;
                    }
                    for (U32 m = 0; args[i] == block && m < type.member_types.size(); ++m)
                    {
                        sizes[m] = DECLARED;
                    }
                }
                return true;
            }

            void count(U32 member, uint32_t index)
            {
                const spirv_cross::SPIRType& member_type = self.get_type(type.member_types[member]);
                if (member_type.array.empty())
                {
                    return; // a vector's or matrix's component
                }
                const spirv_cross::SPIRConstant* constant = self.maybe_get<spirv_cross::SPIRConstant>(index);
                U32& size = sizes[member];
                if (member_type.array.size() > 1 || !constant || constant->specialization || size == DECLARED)
                {
                    size = DECLARED;
                    return;
                }
                size = std::max(size, constant->scalar() + 1);
            }

            Translator& self;
            const U32 block;
            const spirv_cross::SPIRType& type;
            std::map<uint32_t, U32> members; ///< an access chain's result that stops at a member: the member
            std::map<U32, U32> sizes;
        };
    };

    /// What the MSL of a stage expects in its buffers and argument table.
    Reflection reflect(Translator& compiler, const Program& program)
    {
        Reflection reflection;
        const spirv_cross::ShaderResources resources = compiler.get_shader_resources(compiler.get_active_interface_variables());
        for (const spirv_cross::Resource& block : resources.uniform_buffers)
        {
            const spirv_cross::SPIRType& type = compiler.get_type(block.base_type_id);
            const std::string name = compiler.get_name(block.base_type_id);
            // As the MSL struct's sizeof: the declared size rounded up to its 16-byte alignment.
            const U32 size = (U32(compiler.get_declared_struct_size(type)) + 15U) & ~15U;
            if (name != DEFAULT_BLOCK)
            {
                const auto found = std::find(program.blocks.begin(), program.blocks.end(), name);
                reflection.blocks.push_back({ name, size, 1 + U32(found - program.blocks.begin()) });
                continue;
            }
            reflection.defaultBlockSize = size;
            // GL reports only active uniforms, an array at its active size (llrhi.h's ProgramReflection).
            std::vector<bool> active(type.member_types.size(), false);
            for (const spirv_cross::BufferRange& range : compiler.get_active_buffer_ranges(block.id))
            {
                active[range.index] = true;
            }
            const std::map<U32, U32> array_sizes = compiler.activeArraySizes(block.id, type);
            for (U32 i = 0; i < type.member_types.size(); ++i)
            {
                const spirv_cross::SPIRType& member = compiler.get_type(type.member_types[i]);
                Uniform uniform;
                uniform.name = compiler.get_member_name(type.self, i);
                uniform.type = typeName(member);
                uniform.offset = compiler.type_struct_member_offset(type, i);
                uniform.size = U32(compiler.get_declared_struct_member_size(type, i));
                if (!member.array.empty())
                {
                    uniform.arraySize = member.array[0];
                    uniform.arrayStride = compiler.type_struct_member_array_stride(type, i);
                }
                if (member.columns > 1)
                {
                    uniform.matrixStride = compiler.type_struct_member_matrix_stride(type, i);
                }
                uniform.active = active[i];
                if (uniform.active && uniform.arraySize > 0)
                {
                    const auto found = array_sizes.find(i);
                    const U32 read = found == array_sizes.end() ? Translator::DECLARED : found->second;
                    uniform.activeArraySize = std::min(read, uniform.arraySize);
                }
                reflection.uniforms.push_back(uniform);
            }
        }
        for (const spirv_cross::Resource& sampler : resources.sampled_images)
        {
            const U32 index = compiler.get_automatic_msl_resource_binding(sampler.id);
            reflection.textures.push_back({ compiler.get_name(sampler.id), textureType(compiler.get_type(sampler.type_id)), index });
        }
        std::sort(reflection.uniforms.begin(), reflection.uniforms.end(),
                  [](const Uniform& a, const Uniform& b) { return a.offset < b.offset; });
        std::sort(reflection.textures.begin(), reflection.textures.end(),
                  [](const Texture& a, const Texture& b) { return a.index < b.index; });
        return reflection;
    }

    bool fail(Result& result, const std::string& why)
    {
        result.ok = false;
        result.error = why;
        return false;
    }
}

const char* LL::RHI::MSL::stageName(Stage stage)
{
    switch (stage)
    {
    case Stage::Vertex:
        return "vertex";
    case Stage::Fragment:
        return "fragment";
    case Stage::Geometry:
        return "geometry";
    }
    return "unknown";
}

namespace
{
    /// Metal must not fall back where GL did, neither a unit's compile nor the link.
    bool classesHold(const Program& program, Result& result)
    {
        for (const Unit& unit : program.units)
        {
            if (unit.compiledClass < unit.requestedClass)
            {
                return fail(result, llformat("%s compiled only at shader class %d, not %d (R1-14)", unit.file.c_str(),
                                             unit.compiledClass, unit.requestedClass));
            }
        }
        if (program.finalLevel < program.firstLevel)
        {
            return fail(result, llformat("%s linked only at shader level %d, not %d (R1-14)", program.name.c_str(),
                                         program.finalLevel, program.firstLevel));
        }
        return true;
    }
}

LL::RHI::MSL::Result LL::RHI::MSL::translate(const Program& given, const Options& options)
{
    initialize();
    Result result;
    if (!classesHold(given, result))
    {
        return result;
    }
    Program program = given;
    if (options.pointSize)
    {
        // The point variant: the program's vertex main becomes ll_point_main (linkStage's
        // preamble), and a unit of its own calls it and writes the point size from a uniform of the
        // default block.
        program.units.push_back({ POINT_SIZE_UNIT, Stage::Vertex,
                                  "#version 400\nuniform float point_size;\nvoid ll_point_main();\n"
                                  "void main()\n{\n    ll_point_main();\n    gl_PointSize = point_size;\n}\n" });
    }
    for (const Unit& unit : program.units)
    {
        if (unit.stage == Stage::Geometry)
        {
            // Metal has no geometry stage; the program stays with the GL backend.
            result.glOnly = true;
            fail(result, unit.file + " is a geometry stage, which Metal does not have");
            return result;
        }
    }

    std::vector<U32> spirv[2];
    const Stage stages[2] = { Stage::Vertex, Stage::Fragment };
    for (int i = 0; i < 2; ++i)
    {
        spirv[i] = linkStage(program, stages[i], options.pointSize, result.warnings, result.error);
        if (spirv[i].empty())
        {
            result.ok = false;
            return result;
        }
    }

    // SPIRV-Cross reports what it cannot do by throwing.
    try
    {
        Translator vertex(spirv[0]);
        Translator fragment(spirv[1]);
        // Only what each stage uses: GL links a fragment input that the vertex stage does not write as
        // long as nothing reads it, and Metal would not.
        for (spirv_cross::CompilerMSL* compiler : { &vertex, &fragment })
        {
            compiler->set_enabled_interface_variables(compiler->get_active_interface_variables());
        }

        // Vertex inputs at their reserved attribute's index.
        const spirv_cross::ShaderResources vertex_resources = vertex.get_shader_resources(vertex.get_active_interface_variables());
        for (const spirv_cross::Resource& input : vertex_resources.stage_inputs)
        {
            const std::string name = vertex.get_name(input.id);
            const auto found = std::find(program.attributes.begin(), program.attributes.end(), name);
            if (found == program.attributes.end())
            {
                fail(result, "vertex input " + name + " is not a reserved attribute");
                return result;
            }
            vertex.set_decoration(input.id, spv::DecorationLocation, U32(found - program.attributes.begin()));
        }

        // Varyings at the index of their name among the vertex stage's outputs.
        std::vector<std::string> varyings;
        for (const spirv_cross::Resource& output : vertex_resources.stage_outputs)
        {
            varyings.push_back(vertex.get_name(output.id));
        }
        std::sort(varyings.begin(), varyings.end());
        std::map<std::string, U32> location_of;
        U32 next = 0;
        for (const std::string& name : varyings)
        {
            location_of[name] = next;
            // A matrix or array takes a location per column or element.
            const spirv_cross::Resource* output = nullptr;
            for (const spirv_cross::Resource& candidate : vertex_resources.stage_outputs)
            {
                if (vertex.get_name(candidate.id) == name)
                {
                    output = &candidate;
                }
            }
            const spirv_cross::SPIRType& type = vertex.get_type(output->type_id);
            U32 count = std::max(1U, type.columns);
            for (U32 size : type.array)
            {
                count *= std::max(1U, size);
            }
            next += count;
        }
        for (const spirv_cross::Resource& output : vertex_resources.stage_outputs)
        {
            vertex.set_decoration(output.id, spv::DecorationLocation, location_of[vertex.get_name(output.id)]);
        }
        const spirv_cross::ShaderResources fragment_resources = fragment.get_shader_resources(fragment.get_active_interface_variables());
        for (const spirv_cross::Resource& input : fragment_resources.stage_inputs)
        {
            const std::string name = fragment.get_name(input.id);
            const auto found = location_of.find(name);
            if (found == location_of.end())
            {
                fail(result, "fragment input " + name + " has no vertex output");
                return result;
            }
            fragment.set_decoration(input.id, spv::DecorationLocation, found->second);
        }

        // GL gives a fragment stage's only output location 0 and binds none by name; a second one
        // would need its location from GL (glBindFragDataLocation is not used today).
        if (fragment_resources.stage_outputs.size() > 1)
        {
            fail(result, "the fragment stage has more than one output");
            return result;
        }

        StageResult stage_results[2];
        Translator* compilers[2] = { &vertex, &fragment };
        for (int i = 0; i < 2; ++i)
        {
            Translator& compiler = *compilers[i];
            const spirv_cross::ShaderResources resources = compiler.get_shader_resources(compiler.get_active_interface_variables());
            StageResult& stage = stage_results[i];
            stage.stage = stages[i];
            stage.spirv = spirv[i];
            for (const Unit& unit : given.units)
            {
                if (unit.stage == stages[i])
                {
                    stage.classes.push_back(llformat("%s: compiled at class %d, file from class %d", unit.file.c_str(),
                                                     unit.compiledClass, unit.usedClass));
                }
            }
            for (const spirv_cross::Resource& input : resources.stage_inputs)
            {
                stage.inputs.push_back(variable(compiler, input));
            }
            for (const spirv_cross::Resource& output : resources.stage_outputs)
            {
                stage.outputs.push_back(variable(compiler, output));
            }
            auto by_location = [](const Variable& a, const Variable& b) { return a.location < b.location; };
            std::sort(stage.inputs.begin(), stage.inputs.end(), by_location);
            std::sort(stage.outputs.begin(), stage.outputs.end(), by_location);
        }

        // Each fragment input matches its vertex output in type and flat.
        for (const Variable& input : stage_results[1].inputs)
        {
            const auto output = std::find_if(stage_results[0].outputs.begin(), stage_results[0].outputs.end(),
                                             [&input](const Variable& v) { return v.name == input.name; });
            if (output->type != input.type || output->flat != input.flat)
            {
                fail(result, llformat("varying %s is %s%s in the vertex stage and %s%s in the fragment stage", input.name.c_str(),
                                      output->flat ? "flat " : "", output->type.c_str(), input.flat ? "flat " : "", input.type.c_str()));
                return result;
            }
        }

        for (int i = 0; i < 2; ++i)
        {
            Translator& compiler = *compilers[i];
            spirv_cross::CompilerMSL::Options msl;
            msl.platform = spirv_cross::CompilerMSL::Options::macOS;
            msl.set_msl_version(4, 0); // MTLLanguageVersion4_0, as the Metal backend compiles
            msl.enable_point_size_builtin = options.pointSize;
            compiler.set_msl_options(msl);
            if (!bindResources(compiler, program, result.error))
            {
                result.ok = false;
                return result;
            }
            if (stages[i] == Stage::Vertex && options.invariantPosition)
            {
                compiler.makePositionInvariant();
            }
            if (stages[i] == Stage::Vertex)
            {
                // GL's clip space and row order, corrected once at the end of the vertex stage.
                spirv_cross::CompilerGLSL::Options common = compiler.get_common_options();
                common.vertex.fixup_clipspace = true;
                common.vertex.flip_vert_y = true;
                compiler.set_common_options(common);
            }
            try
            {
                compiler.declareDepthImages();
                stage_results[i].msl = compiler.compile();
                stage_results[i].reflection = reflect(compiler, program);
            }
            catch (const spirv_cross::CompilerError& e)
            {
                fail(result, std::string(stageName(stages[i])) + " stage to MSL: " + e.what());
                return result;
            }
            result.stages.push_back(stage_results[i]);
        }
    }
    catch (const std::exception& e)
    {
        fail(result, std::string("SPIRV-Cross: ") + e.what());
        return result;
    }
    result.ok = true;
    return result;
}

namespace
{
    /// The tools and this translation, in every cache key: a new version of either, or a change
    /// to how the translation works, makes new entries.
    std::string toolVersions()
    {
        const glslang::Version glslang = glslang::GetVersion();
        return llformat("glslang %d.%d.%d%s; SPIRV-Cross %s; llrhimsl %d; GLSL %d; MSL 4.0", glslang.major, glslang.minor,
                        glslang.patch, glslang.flavor, SPIRV_CROSS_RELEASE, TRANSLATION_VERSION, GLSL_VERSION);
    }

    LLSD toLLSD(const std::vector<Variable>& variables)
    {
        LLSD list = LLSD::emptyArray();
        for (const Variable& v : variables)
        {
            list.append(llsd::map("name", v.name, "type", v.type, "location", (S32)v.location, "flat", v.flat));
        }
        return list;
    }

    std::vector<Variable> variablesFrom(const LLSD& list)
    {
        std::vector<Variable> variables;
        for (const LLSD& v : llsd::inArray(list))
        {
            variables.push_back({ v["name"].asString(), v["type"].asString(), (U32)v["location"].asInteger(), v["flat"].asBoolean() });
        }
        return variables;
    }
}

LL::RHI::MSL::DiskCache::DiskCache(const std::string& folder)
    : mFolder(folder)
{
    LLFile::mkdir(folder);
}

namespace
{
    std::string generationFolder(const std::string& generation)
    {
        HBXXH128 hash;
        hash.update(generation);
        return "generation-" + hash.digest().asString();
    }
}

LL::RHI::MSL::DiskCache::DiskCache(const std::string& root, const std::string& generation)
    : mFolder(root + "/" + generationFolder(generation))
{
    LLFile::mkdir(root);
    // Listed first and removed after, so the walk never meets a folder it is removing. An entry that cannot be
    // removed stays: it costs space, never a wrong translation.
    const std::string own = generationFolder(generation);
    std::vector<std::filesystem::path> others;
    std::error_code error;
    for (std::filesystem::directory_iterator it(root, error), end; !error && it != end; it.increment(error))
    {
        if (it->path().filename().string() != own)
        {
            others.push_back(it->path());
        }
    }
    for (const std::filesystem::path& other : others)
    {
        std::filesystem::remove_all(other, error);
    }
    LLFile::mkdir(mFolder);
}

std::string LL::RHI::MSL::DiskCache::key(const Program& program, const Options& options, const std::string& capabilities) const
{
    HBXXH128 hash;
    auto add = [&hash](const std::string& text)
    {
        // With its length, so that no two lists give the same bytes.
        hash.update(llformat("%zu:", text.size()));
        hash.update(text);
    };
    add(toolVersions());
    add(capabilities);
    add(options.pointSize ? "point size" : "no point size");
    if (options.invariantPosition)
    {
        add("invariant position"); // only then, so the keys without it stay as they were
    }
    for (const Unit& unit : program.units)
    {
        add(stageName(unit.stage));
        add(unit.file);
        add(unit.source);
        add(llformat("classes %d %d %d", unit.requestedClass, unit.compiledClass, unit.usedClass));
    }
    for (const std::string& attribute : program.attributes)
    {
        add("attribute " + attribute);
    }
    for (const std::string& block : program.blocks)
    {
        add("block " + block);
    }
    add(llformat("levels %d %d", program.firstLevel, program.finalLevel));
    return hash.digest().asString();
}

bool LL::RHI::MSL::DiskCache::load(const std::string& key, Result& result) const
{
    llifstream file((mFolder + "/" + key + ".llsd").c_str(), std::ios::binary);
    LLSD data;
    if (!file.is_open() || LLSDSerialize::fromNotation(data, file, LLSDSerialize::SIZE_UNLIMITED) <= 0)
    {
        return false;
    }
    result = Result();
    result.ok = true;
    for (const LLSD& warning : llsd::inArray(data["warnings"]))
    {
        result.warnings.push_back(warning.asString());
    }
    for (const LLSD& entry : llsd::inArray(data["stages"]))
    {
        StageResult stage;
        stage.stage = Stage(entry["stage"].asInteger());
        stage.msl = entry["msl"].asString();
        stage.inputs = variablesFrom(entry["inputs"]);
        stage.outputs = variablesFrom(entry["outputs"]);
        for (const LLSD& c : llsd::inArray(entry["classes"]))
        {
            stage.classes.push_back(c.asString());
        }
        const LLSD& reflection = entry["reflection"];
        stage.reflection.defaultBlockSize = (U32)reflection["default_block_size"].asInteger();
        for (const LLSD& u : llsd::inArray(reflection["uniforms"]))
        {
            stage.reflection.uniforms.push_back({ u["name"].asString(), u["type"].asString(), (U32)u["offset"].asInteger(),
                                                  (U32)u["size"].asInteger(), (U32)u["array_size"].asInteger(),
                                                  (U32)u["array_stride"].asInteger(), (U32)u["matrix_stride"].asInteger(),
                                                  u["active"].asBoolean(), (U32)u["active_array_size"].asInteger() });
        }
        for (const LLSD& b : llsd::inArray(reflection["blocks"]))
        {
            stage.reflection.blocks.push_back({ b["name"].asString(), (U32)b["size"].asInteger(), (U32)b["buffer"].asInteger() });
        }
        for (const LLSD& t : llsd::inArray(reflection["textures"]))
        {
            stage.reflection.textures.push_back({ t["name"].asString(), t["type"].asString(), (U32)t["index"].asInteger() });
        }
        result.stages.push_back(stage);
    }
    return result.stages.size() == 2;
}

bool LL::RHI::MSL::DiskCache::store(const std::string& key, const Result& result) const
{
    if (!result.ok)
    {
        return false;
    }
    LLSD data;
    data["tools"] = toolVersions();
    data["warnings"] = LLSD::emptyArray();
    for (const std::string& warning : result.warnings)
    {
        data["warnings"].append(warning);
    }
    data["stages"] = LLSD::emptyArray();
    for (const StageResult& stage : result.stages)
    {
        LLSD entry = llsd::map("stage", (S32)stage.stage, "msl", stage.msl, "inputs", toLLSD(stage.inputs), "outputs",
                               toLLSD(stage.outputs));
        LLSD reflection = llsd::map("default_block_size", (S32)stage.reflection.defaultBlockSize, "uniforms", LLSD::emptyArray(),
                                    "blocks", LLSD::emptyArray(), "textures", LLSD::emptyArray());
        for (const Uniform& u : stage.reflection.uniforms)
        {
            reflection["uniforms"].append(llsd::map("name", u.name, "type", u.type, "offset", (S32)u.offset, "size", (S32)u.size,
                                                    "array_size", (S32)u.arraySize, "array_stride", (S32)u.arrayStride,
                                                    "matrix_stride", (S32)u.matrixStride, "active", u.active, "active_array_size",
                                                    (S32)u.activeArraySize));
        }
        for (const Block& b : stage.reflection.blocks)
        {
            reflection["blocks"].append(llsd::map("name", b.name, "size", (S32)b.size, "buffer", (S32)b.buffer));
        }
        for (const Texture& t : stage.reflection.textures)
        {
            reflection["textures"].append(llsd::map("name", t.name, "type", t.type, "index", (S32)t.index));
        }
        entry["reflection"] = reflection;
        entry["classes"] = LLSD::emptyArray();
        for (const std::string& c : stage.classes)
        {
            entry["classes"].append(c);
        }
        data["stages"].append(entry);
    }
    // Written aside and renamed, so that a reader never sees half an entry.
    const std::string path = mFolder + "/" + key + ".llsd";
    static std::atomic<U32> writes{ 0 }; // threads of one process write files of their own
    const std::string temporary = path + llformat(".%u.%u.tmp", (U32)getpid(), writes++);
    llofstream file(temporary.c_str(), std::ios::binary);
    LLSDSerialize::toNotation(data, file);
    file.close();
    if (file.fail() || LLFile::rename(temporary, path) != 0)
    {
        LLFile::remove(temporary); // our own half-written file
        return false;
    }
    return true;
}

LL::RHI::MSL::Result LL::RHI::MSL::DiskCache::translate(const Program& program, const Options& options,
                                                        const std::string& capabilities, bool* hit) const
{
    Result result;
    if (!classesHold(program, result))
    {
        return result;
    }
    const std::string entry = key(program, options, capabilities);
    if (load(entry, result))
    {
        if (hit)
        {
            *hit = true;
        }
        return result;
    }
    if (hit)
    {
        *hit = false;
    }
    result = LL::RHI::MSL::translate(program, options);
    store(entry, result);
    return result;
}
