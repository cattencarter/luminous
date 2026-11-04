/**
 * @file llrhimsl_test.cpp
 * @brief Tests of the translation of the viewer's GLSL programs to MSL (P1-04c)
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

#include "../llrhimsl.h"

#include "llfile.h"

#include "../test/lltut.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <unistd.h>

using namespace LL::RHI::MSL;

/// Compiles MSL with Metal at language version 4.0 (tests/llshadertoolchain_metal.mm); empty, or why.
std::string compileWithMetal(const std::string& msl);

namespace
{
    /// This process's own scratch folder under $TMPDIR, made on first use and removed at exit, as llrhigl_test's
    /// tempFolder: the checkouts of one user share $TMPDIR and run their unit tests at the same time
    /// (P3-T3).
    struct TempFolder
    {
        const std::string mPath = std::string(getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp") + llformat("/llrhimsl.%d", int(getpid()));

        TempFolder()
        {
            // Empty first: a test process that aborted (a validation assert, SIGKILL, a timeout) leaves its folder, and a
            // later process with the same pid would read its files (P3-T3's review m2).
            std::error_code error;
            std::filesystem::remove_all(mPath, error);
            std::filesystem::create_directories(mPath, error);
        }

        ~TempFolder()
        {
            std::error_code error;
            std::filesystem::remove_all(mPath, error);
        }
    };

    std::string tempFolder()
    {
        static const TempFolder folder;
        return folder.mPath;
    }

    // As LLShaderMgr::mReservedAttribs: position 0, normal 1, texcoord0 2, ..., diffuse_color 6.
    const std::vector<std::string> ATTRIBUTES = { "position", "normal", "texcoord0", "texcoord1", "texcoord2",
                                                  "texcoord3", "diffuse_color", "emissive", "tangent", "weight",
                                                  "weight4", "clothing", "joint", "texture_index" };

    // A vertex stage of two units, the second defining a function the first calls, and a fragment
    // stage that declares its inputs in the other order, as the viewer's uiV/uiF do (R1-01).
    const char* VERTEX_MAIN = R"(#version 400
#define VERTEX_SHADER 1
uniform mat4 modelview_projection_matrix;
in vec3 position;
in vec4 diffuse_color;
in vec2 texcoord0;
out vec4 vertex_color;
out vec2 vary_texcoord0;
vec4 tint(vec4 c);
void main()
{
    gl_Position = modelview_projection_matrix * vec4(position, 1.0);
    vertex_color = tint(diffuse_color);
    vary_texcoord0 = texcoord0;
}
)";
    const char* VERTEX_HELPER = R"(#version 400
#define VERTEX_SHADER 1
uniform vec4 tint_color;
vec4 tint(vec4 c) { return c * tint_color; }
)";
    const char* FRAGMENT = R"(#version 400
#define FRAGMENT_SHADER 1
uniform sampler2D diffuseMap;
in vec2 vary_texcoord0;
in vec4 vertex_color;
out vec4 frag_color;
void main()
{
    frag_color = texture(diffuseMap, vary_texcoord0) * vertex_color;
}
)";

    Program uiProgram()
    {
        Program program;
        program.name = "test UI";
        program.attributes = ATTRIBUTES;
        program.blocks = { "ReflectionProbes", "GLTFJoints", "GLTFNodes", "GLTFMaterials" };
        program.units = { { "test/uiV.glsl", Stage::Vertex, VERTEX_MAIN },
                          { "test/uiHelperV.glsl", Stage::Vertex, VERTEX_HELPER },
                          { "test/uiF.glsl", Stage::Fragment, FRAGMENT } };
        return program;
    }

    /// One vertex and one fragment unit, for the reflection's tests (P3-01d).
    Program programOf(const char* vertex, const char* fragment)
    {
        Program program;
        program.name = "test reflection";
        program.attributes = ATTRIBUTES;
        program.units = { { "test/reflectionV.glsl", Stage::Vertex, vertex }, { "test/reflectionF.glsl", Stage::Fragment, fragment } };
        return program;
    }

    const char* PLAIN_FRAGMENT = R"(#version 400
out vec4 frag_color;
void main() { frag_color = vec4(1.0); }
)";

    const Uniform* findUniform(const StageResult& stage, const std::string& name)
    {
        for (const Uniform& uniform : stage.reflection.uniforms)
        {
            if (uniform.name == name)
            {
                return &uniform;
            }
        }
        return nullptr;
    }

    const Variable* find(const std::vector<Variable>& variables, const std::string& name)
    {
        for (const Variable& variable : variables)
        {
            if (variable.name == name)
            {
                return &variable;
            }
        }
        return nullptr;
    }
}

namespace tut
{
    struct rhimsl_data
    {
    };
    typedef test_group<rhimsl_data> rhimsl_group;
    typedef rhimsl_group::object object;
    rhimsl_group rhimsl_test_group("rhi msl");

    template<> template<>
    void object::test<1>()
    {
        set_test_name("units link per stage, attributes at their reserved index, varyings matched by name");
        const Result result = translate(uiProgram());
        ensure("translated: " + result.error, result.ok);
        ensure_equals("two stages", result.stages.size(), size_t(2));
        const StageResult& vertex = result.stages[0];
        const StageResult& fragment = result.stages[1];
        ensure_equals("position at 0", find(vertex.inputs, "position")->location, 0U);
        ensure_equals("texcoord0 at 2", find(vertex.inputs, "texcoord0")->location, 2U);
        ensure_equals("diffuse_color at 6", find(vertex.inputs, "diffuse_color")->location, 6U);
        for (const char* name : { "vertex_color", "vary_texcoord0" })
        {
            ensure(std::string(name) + " in both stages", find(vertex.outputs, name) && find(fragment.inputs, name));
            ensure_equals(std::string(name) + " at one location", find(fragment.inputs, name)->location,
                          find(vertex.outputs, name)->location);
        }
        ensure_contains("vertex MSL", vertex.msl, "vertex main0_out main0(");
        ensure_contains("the loose uniforms in the default block", vertex.msl, "DefaultUniforms");
        ensure("the default block in buffer 0", vertex.msl.find("constant DefaultUniforms&") != std::string::npos
                                                 && vertex.msl.find("[[buffer(0)]]") != std::string::npos);
        ensure_contains("clip space corrected at the end", vertex.msl,
                        "out.gl_Position.z = (out.gl_Position.z + out.gl_Position.w) * 0.5");
        ensure_contains("and y flipped", vertex.msl, "out.gl_Position.y = -(out.gl_Position.y)");
        ensure_contains("fragment MSL", fragment.msl, "fragment main0_out main0(");
        ensure_equals("Metal compiles the vertex stage", compileWithMetal(vertex.msl), std::string());
        ensure_equals("and the fragment stage", compileWithMetal(fragment.msl), std::string());

        // The default block as the MSL expects it: a mat4 and a vec4 (R1-07).
        const Reflection& reflection = vertex.reflection;
        ensure_equals("two loose uniforms in the vertex stage", reflection.uniforms.size(), size_t(2));
        const Uniform* matrix = nullptr;
        const Uniform* tint = nullptr;
        for (const Uniform& uniform : reflection.uniforms)
        {
            (uniform.name == "modelview_projection_matrix" ? matrix : tint) = &uniform;
        }
        ensure("both found", matrix && tint && tint->name == "tint_color");
        ensure_equals("the matrix is a float4x4", matrix->type, std::string("float4x4"));
        ensure_equals("16 bytes a column", matrix->matrixStride, 16U);
        ensure_equals("64 bytes", matrix->size, 64U);
        ensure_equals("the vec4 is 16 bytes", tint->size, 16U);
        ensure_equals("the block holds both", reflection.defaultBlockSize, 80U);
        ensure_equals("one texture in the fragment stage", fragment.reflection.textures.size(), size_t(1));
        ensure_equals("diffuseMap", fragment.reflection.textures[0].name, std::string("diffuseMap"));
        ensure_equals("at index 0", fragment.reflection.textures[0].index, 0U);
        ensure_equals("a 2D texture", fragment.reflection.textures[0].type, std::string("2d"));
        ensure_contains("at texture 0 in the MSL", fragment.msl, "[[texture(0)]]");
    }

    template<> template<>
    void object::test<2>()
    {
        set_test_name("a fragment input without a vertex output, a geometry stage and a broken unit fail with a reason");
        Program program = uiProgram();
        std::string source = FRAGMENT;
        const std::string declaration = "in vec4 vertex_color;";
        const std::string use = "* vertex_color";
        source.replace(source.find(declaration), declaration.size(), "in vec4 vertex_colour;");
        source.replace(source.find(use), use.size(), "* vertex_colour");
        program.units[2].source = source;
        Result result = translate(program);
        ensure("fails", !result.ok);
        ensure_contains("names the input", result.error, "vertex_colour");

        program = uiProgram();
        program.units.push_back({ "test/normaldebugG.glsl", Stage::Geometry, "#version 400\nvoid main() {}\n" });
        result = translate(program);
        ensure("GL only", !result.ok && result.glOnly);

        program = uiProgram();
        program.units[1].source += "this is not GLSL";
        result = translate(program);
        ensure("fails", !result.ok);
        ensure_contains("glslang's log names the file", result.error, "test/uiHelperV.glsl");
    }

    template<> template<>
    void object::test<3>()
    {
        set_test_name("the point variant writes the point size from a uniform; an invariant position (P3-04a S4); a class "
                      "fall-back fails");
        Options options;
        options.pointSize = true;
        Result result = translate(uiProgram(), options);
        ensure("translated: " + result.error, result.ok);
        const StageResult& vertex = result.stages[0];
        ensure_contains("a point size output", vertex.msl, "[[point_size]]");
        bool found = false;
        for (const Uniform& uniform : vertex.reflection.uniforms)
        {
            found = found || uniform.name == "point_size";
        }
        ensure("point_size in the default block", found);
        ensure_equals("Metal compiles it", compileWithMetal(vertex.msl), std::string());
        ensure("no point size without the variant", translate(uiProgram()).stages[0].msl.find("[[point_size]]") == std::string::npos);
        Options invariant;
        invariant.invariantPosition = true;
        result = translate(uiProgram(), invariant);
        ensure("translated invariant: " + result.error, result.ok);
        ensure_contains("an invariant position", result.stages[0].msl, "[[position, invariant]]");
        ensure_equals("Metal compiles it", compileWithMetal(result.stages[0].msl), std::string());
        ensure("not invariant without the option",
               translate(uiProgram()).stages[0].msl.find("invariant") == std::string::npos);

        Program program = uiProgram();
        program.units[0].requestedClass = 3;
        program.units[0].compiledClass = 2;
        result = translate(program);
        ensure("fails", !result.ok);
        ensure_contains("says why", result.error, "R1-14");

        program = uiProgram();
        program.firstLevel = 3;
        program.finalLevel = 2;
        result = translate(program);
        ensure("a fall-back at link fails too", !result.ok && result.error.find("R1-14") != std::string::npos);
    }

    template<> template<>
    void object::test<4>()
    {
        set_test_name("the disk cache gives back what it stored, and a changed unit gets a new key");
        const std::string folder = tempFolder() + "/cache";
        DiskCache cache(folder);
        Program program = uiProgram();
        program.units[1].source += llformat("\n// %u\n", (U32)getpid()); // a key no earlier run made
        bool hit = true;
        const Result first = cache.translate(program, Options(), "test capabilities", &hit);
        ensure("translated: " + first.error, first.ok);
        ensure("not from the cache the first time", !hit);
        const Result second = cache.translate(program, Options(), "test capabilities", &hit);
        ensure("from the cache the second time", hit && second.ok);
        ensure_equals("the same vertex MSL", second.stages[0].msl, first.stages[0].msl);
        ensure_equals("the same fragment MSL", second.stages[1].msl, first.stages[1].msl);
        ensure_equals("the same reflection", second.stages[0].reflection.uniforms.size(), first.stages[0].reflection.uniforms.size());
        ensure_equals("and offsets", second.stages[0].reflection.uniforms.back().offset, first.stages[0].reflection.uniforms.back().offset);
        ensure_equals("the same locations", second.stages[1].inputs.size(), first.stages[1].inputs.size());
        ensure("and the classes", second.stages[0].classes == first.stages[0].classes && !first.stages[0].classes.empty());
        size_t active = 0;
        for (size_t s = 0; s < first.stages.size(); ++s)
        {
            for (const Uniform& stored : first.stages[s].reflection.uniforms)
            {
                const Uniform* loaded = findUniform(second.stages[s], stored.name);
                ensure(stored.name + " is loaded", loaded != nullptr);
                ensure_equals(stored.name + "'s active", loaded->active, stored.active);
                ensure_equals(stored.name + "'s activeArraySize", loaded->activeArraySize, stored.activeArraySize);
                active += stored.active ? 1 : 0;
            }
        }
        ensure("an active uniform is stored", active > 0);
        const std::string key = cache.key(program, Options(), "test capabilities");
        ensure("other capabilities, another key", cache.key(program, Options(), "other") != key);
        Options points;
        points.pointSize = true;
        ensure("the point variant, another key", cache.key(program, points, "test capabilities") != key);
        Options invariant;
        invariant.invariantPosition = true;
        ensure("an invariant position, another key", cache.key(program, invariant, "test capabilities") != key);
        program.units[0].source += "\n";
        ensure("a changed unit, another key", cache.key(program, Options(), "test capabilities") != key);
        Program fallen = uiProgram();
        fallen.units[1].source = program.units[1].source;
        fallen.units[1].requestedClass = 3;
        fallen.units[1].compiledClass = 2;
        ensure("the classes are in the key", cache.key(fallen, Options(), "test capabilities") != key);
        ensure("and a fall-back fails through the cache", !cache.translate(fallen, Options(), "test capabilities", &hit).ok);
        LLFile::remove(folder + "/" + key + ".llsd");
    }

    template<> template<>
    void object::test<5>()
    {
        set_test_name("a loose uniform the stage does not read is not active (P3-01d)");
        const Result result = translate(programOf(R"(#version 400
uniform vec4 used;
uniform vec4 unused;
in vec3 position;
void main() { gl_Position = vec4(position, 1.0) * used; }
)", PLAIN_FRAGMENT));
        ensure("translated: " + result.error, result.ok && result.stages.size() == 2);
        const Uniform* used = findUniform(result.stages[0], "used");
        const Uniform* unused = findUniform(result.stages[0], "unused");
        ensure("both are in the default block", used && unused);
        ensure("used is active", used->active);
        ensure("unused is not active", !unused->active);
        ensure_equals("not an array", used->activeArraySize, 0u);
    }

    template<> template<>
    void object::test<6>()
    {
        set_test_name("an array's active size is its highest constant index + 1, else its declared size (P3-01d E17)");
        const Result result = translate(programOf(R"(#version 400
uniform float a[8];
uniform vec4 b[4];
uniform vec4 c[3];
uniform vec4 d[5];
uniform vec4 e[6];
uniform int i;
in vec3 position;
float sum(vec4 values[3]) { return values[0].x + values[1].y; }
float called() { return d[3].x; }
float never() { return e[5].x; }
void main()
{
    gl_Position = vec4(position, a[0] + a[2]) + b[i] + vec4(sum(c)) + vec4(called()) + e[1];
}
)", PLAIN_FRAGMENT));
        ensure("translated: " + result.error, result.ok && result.stages.size() == 2);
        const std::pair<const char*, U32> expected[] = {
            { "a", 3 }, // a[0] and a[2]
            { "b", 4 }, // b[i]: the declared size
            { "c", 3 }, // the whole array to a function: the declared size
            { "d", 4 }, // d[3] in a function main calls
            { "e", 2 }, // e[1] in main; e[5] only in a function no one calls
        };
        for (const auto& [name, size] : expected)
        {
            const Uniform* uniform = findUniform(result.stages[0], name);
            ensure(std::string(name) + " is in the default block", uniform != nullptr);
            ensure(std::string(name) + " is active", uniform->active);
            ensure_equals(std::string(name) + "'s activeArraySize", uniform->activeArraySize, size);
        }
        ensure_equals("i is not an array", findUniform(result.stages[0], "i")->activeArraySize, 0u);
    }

    template<> template<>
    void object::test<7>()
    {
        set_test_name("a shadow sampler's texture is Metal's depth type, in main and in a function that takes it (P3-02 D2)");
        // SPIRV-Cross alone declares texture2d and casts it to depth2d at each comparison, which Metal's shader
        // validation reads as a resource usage mismatch giving 0 (tasks/P3-02.md, S3b).
        const Result result = translate(programOf(R"(#version 400
in vec3 position;
void main() { gl_Position = vec4(position, 1.0); }
)", R"(#version 400
uniform sampler2D plain;
uniform sampler2DShadow flat2D;
uniform sampler2DArrayShadow layered;
uniform samplerCubeShadow cube;
uniform samplerCubeArrayShadow cubes;
out vec4 frag_color;
float lit(sampler2DShadow map, vec2 uv) { return texture(map, vec3(uv, 0.5)); }
void main()
{
    frag_color = texture(plain, vec2(0.5)) * (lit(flat2D, vec2(0.5)) + texture(layered, vec4(0.5, 0.5, 1.0, 0.5))
                 + texture(cube, vec4(1.0, 0.0, 0.0, 0.5)) + texture(cubes, vec4(1.0, 0.0, 0.0, 1.0), 0.5));
}
)"));
        ensure("translated: " + result.error, result.ok && result.stages.size() == 2);
        const std::string& msl = result.stages[1].msl;
        for (const char* declared : { "texture2d<float> plain [[", "depth2d<float> flat2D [[", "depth2d_array<float> layered [[",
                                      "depthcube<float> cube [[", "depthcube_array<float> cubes [[", "depth2d<float> map," })
        {
            ensure(std::string("the MSL declares ") + declared + "\n" + msl, msl.find(declared) != std::string::npos);
        }
        for (const char* cast : { "texture2d<float> flat2D", "texture2d_array<float> layered", "texturecube<float> cube",
                                  "texturecube_array<float> cubes", "texture2d<float> map" })
        {
            ensure(std::string("no color type: ") + cast, msl.find(cast) == std::string::npos);
        }
        ensure_equals("Metal compiles it", compileWithMetal(msl), std::string());
    }

    template<> template<>
    void object::test<8>()
    {
        set_test_name("a generation's cache is a folder of its own; another generation's folder and files go when a cache is made (P3-R4, B-2)");
        namespace fs = std::filesystem;
        const std::string root = tempFolder() + "/generations";
        const DiskCache first(root, "build A");
        ensure_equals("the generation's folder is under the root", fs::path(first.folder()).parent_path().string(), root);
        Program program = uiProgram();
        program.units[1].source += llformat("\n// t8 %u\n", (U32)getpid());
        bool hit = true;
        ensure("translated into A", first.translate(program, Options(), "build A", &hit).ok && !hit);
        const std::string entry = first.folder() + "/" + first.key(program, Options(), "build A") + ".llsd";
        ensure("the entry is in A's folder", fs::exists(entry));
        {
            std::ofstream stray(root + "/stray.llsd");
        }
        const DiskCache again(root, "build A");
        ensure_equals("the same generation, the same folder", again.folder(), first.folder());
        ensure("A's entry stays", fs::exists(entry));
        ensure("and is a hit", again.translate(program, Options(), "build A", &hit).ok && hit);
        ensure("a stray file goes", !fs::exists(root + "/stray.llsd"));
        const DiskCache second(root, "build B");
        ensure("another generation, another folder", second.folder() != first.folder());
        ensure("A's folder goes", !fs::exists(first.folder()));
        ensure("B's folder is made", fs::is_directory(second.folder()));
        std::error_code error;
        fs::remove_all(root, error);
    }
    template<> template<>
    void object::test<9>()
    {
        set_test_name("a sampler3D is Metal's texture3d, reflected as a 3d texture, beside a 2D one");
        // The Look's tables in the tonemap programs: two 3D lookups sampled with a float3, beside the frame's 2D texture.
        const Result result = translate(programOf(R"(#version 400
in vec3 position;
void main() { gl_Position = vec4(position, 1.0); }
)", R"(#version 400
uniform sampler2D diffuseRect;
uniform sampler3D luminous_tonemap_lut;
uniform sampler3D luminous_lut;
uniform float luminous_lut_size;
out vec4 frag_color;
vec3 lookup(sampler3D table, vec3 c, float size) { return texture(table, c * ((size - 1.0) / size) + 0.5 / size).rgb; }
void main()
{
    vec3 c = texture(diffuseRect, vec2(0.5)).rgb;
    c = lookup(luminous_lut, c, luminous_lut_size);
    frag_color = vec4(texture(luminous_tonemap_lut, c).rgb, 1.0);
}
)"));
        ensure("translated: " + result.error, result.ok && result.stages.size() == 2);
        const std::string& msl = result.stages[1].msl;
        for (const char* declared : { "texture3d<float> luminous_tonemap_lut [[", "texture3d<float> luminous_lut [[",
                                      "texture2d<float> diffuseRect [[" })
        {
            ensure(std::string("the MSL declares ") + declared + "\n" + msl, msl.find(declared) != std::string::npos);
        }
        const std::vector<Texture>& textures = result.stages[1].reflection.textures;
        ensure_equals("three textures", textures.size(), size_t(3));
        U32 volumes = 0;
        for (const Texture& texture : textures)
        {
            const bool lut = texture.name == "luminous_tonemap_lut" || texture.name == "luminous_lut";
            ensure_equals(texture.name + "'s type", texture.type, std::string(lut ? "3d" : "2d"));
            volumes += lut ? 1 : 0;
        }
        ensure_equals("both tables are 3d", volumes, 2U);
        ensure_equals("Metal compiles it", compileWithMetal(msl), std::string());
    }
}
