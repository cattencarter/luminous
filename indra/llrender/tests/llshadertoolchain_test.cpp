/**
 * @file llshadertoolchain_test.cpp
 * @brief The shader tool chain's libraries link and translate: GLSL to SPIR-V to MSL, which Metal compiles
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

#include "glslang/Public/ResourceLimits.h"
#include "glslang/Public/ShaderLang.h"
#include "SPIRV/GlslangToSpv.h"
#include "spirv_msl.hpp"

#include "../test/lltut.h"

#include <string>
#include <vector>

// Only that glslang and SPIRV-Cross link from llrender's directory and work end to end, as far as
// Metal compiling their MSL (P1-04a). The translation of the viewer's own GLSL, with its loose
// uniforms and defines, and SPIRV-Cross's MSL target version are P1-04c's (decision 13).

/// Compiles MSL with Metal at language version 4.0 (decision 13); empty, or why it failed.
std::string compileWithMetal(const std::string& msl);

namespace
{
    const char* VERTEX = R"(#version 450
layout(location = 0) in vec3 position;
layout(location = 1) in vec4 diffuse_color;
layout(location = 0) out vec4 vertex_color;
layout(std140, set = 0, binding = 0) uniform Transform
{
    mat4 modelview_projection_matrix;
};
void main()
{
    gl_Position = modelview_projection_matrix * vec4(position, 1.0);
    vertex_color = diffuse_color;
}
)";

    const char* FRAGMENT = R"(#version 450
layout(location = 0) in vec4 vertex_color;
layout(set = 0, binding = 1) uniform sampler2D diffuseMap;
layout(location = 0) out vec4 frag_color;
void main()
{
    frag_color = texture(diffuseMap, vec2(0.5)) * vertex_color;
}
)";

    /// SPIR-V from glslang, or empty with the log in why.
    std::vector<unsigned int> toSpirv(EShLanguage stage, const char* source, std::string& why)
    {
        glslang::TShader shader(stage);
        shader.setStrings(&source, 1);
        shader.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan, 100);
        shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_1);
        shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_3);
        if (!shader.parse(GetDefaultResources(), 100, false, EShMsgDefault))
        {
            why = shader.getInfoLog();
            return {};
        }
        glslang::TProgram program;
        program.addShader(&shader);
        if (!program.link(EShMsgDefault))
        {
            why = program.getInfoLog();
            return {};
        }
        std::vector<unsigned int> spirv;
        glslang::GlslangToSpv(*program.getIntermediate(stage), spirv);
        return spirv;
    }

    std::string toMsl(std::vector<unsigned int> spirv)
    {
        spirv_cross::CompilerMSL msl(std::move(spirv));
        spirv_cross::CompilerMSL::Options options;
        options.platform = spirv_cross::CompilerMSL::Options::macOS;
        options.set_msl_version(4, 0); // for this test; P1-04c chooses it
        msl.set_msl_options(options);
        return msl.compile();
    }
}

namespace tut
{
    struct shadertoolchain_data
    {
        shadertoolchain_data() { glslang::InitializeProcess(); }
        ~shadertoolchain_data() { glslang::FinalizeProcess(); }
    };
    typedef test_group<shadertoolchain_data> shadertoolchain_group;
    typedef shadertoolchain_group::object object;
    shadertoolchain_group shadertoolchain_test_group("shader tool chain");

    template<> template<>
    void object::test<1>()
    {
        set_test_name("glslang makes SPIR-V, SPIRV-Cross makes MSL and reflects it, and Metal compiles the MSL");
        std::string why;
        std::vector<unsigned int> vertex = toSpirv(EShLangVertex, VERTEX, why);
        ensure("vertex SPIR-V: " + why, !vertex.empty());
        ensure_equals("SPIR-V's magic number", vertex[0], 0x07230203u);
        std::vector<unsigned int> fragment = toSpirv(EShLangFragment, FRAGMENT, why);
        ensure("fragment SPIR-V: " + why, !fragment.empty());

        const std::string vertexMsl = toMsl(vertex);
        ensure_contains("a vertex function", vertexMsl, "vertex main0_out main0(");
        ensure_contains("with its uniform block", vertexMsl, "constant Transform&");
        const std::string fragmentMsl = toMsl(fragment);
        ensure_contains("a fragment function", fragmentMsl, "fragment main0_out main0(");
        ensure_contains("with its texture", fragmentMsl, "texture2d<float> diffuseMap");
        ensure_equals("Metal compiles the vertex function", compileWithMetal(vertexMsl), std::string());
        ensure_equals("and the fragment function", compileWithMetal(fragmentMsl), std::string());

        spirv_cross::Compiler reflection(vertex);
        const spirv_cross::ShaderResources resources = reflection.get_shader_resources();
        ensure_equals("one uniform block", resources.uniform_buffers.size(), size_t(1));
        ensure_equals("named", resources.uniform_buffers[0].name, std::string("Transform"));
        ensure_equals("two inputs", resources.stage_inputs.size(), size_t(2));
    }

    template<> template<>
    void object::test<2>()
    {
        set_test_name("GLSL that does not compile gives glslang's log");
        std::string why;
        const std::vector<unsigned int> spirv = toSpirv(EShLangFragment, "#version 450\nvoid main() { nonsense; }\n", why);
        ensure("no SPIR-V", spirv.empty());
        ensure_contains("the log names the error", why, "nonsense");
        ensure("and Metal rejects what is not MSL", !compileWithMetal("this is not MSL").empty());
    }
}
