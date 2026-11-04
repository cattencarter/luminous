/**
 * @file llrhimsl.h
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

#ifndef LL_LLRHIMSL_H
#define LL_LLRHIMSL_H

#include "stdtypes.h"

#include <string>
#include <vector>

// [Viewer Lab] Darwin only. A program is what the GL viewer links: per stage its shader objects
// in attach order, each with the source the shader manager assembled (defines included), as
// LLShaderMgr hands them to glShaderSource and as its manifest records them. Each stage of a
// program becomes one glslang program of its units with Vulkan's relaxed rules, the loose uniforms
// gathered into the stage's default block. Locations are set before MSL: vertex inputs at their
// index among the reserved attributes, which is LLVertexBuffer's attribute type, and varyings at
// the index of their name among the vertex stage's outputs, so that both stages agree whatever
// order they declare them in. Resources get fixed MSL indices: the default block buffer 0, the
// other uniform blocks buffer 1 + GL's binding, and each stage's samplers texture and sampler i in
// the order of their names; buffers 16-30 are left to the vertex streams.

namespace LL::RHI::MSL
{
    enum class Stage
    {
        Vertex,
        Fragment,
        Geometry,
    };

    const char* stageName(Stage stage);

    /// One shader object of a program, as the GL viewer compiled it.
    struct Unit
    {
        std::string file;   ///< relative to the shaders folder, e.g. "deferred/pbropaqueF.glsl"
        Stage stage = Stage::Vertex;
        std::string source; ///< assembled, after the defines
        S32 requestedClass = 0; ///< the shader class asked for (as the shader manager's manifest records it); 0 when not known
        S32 compiledClass = 0;  ///< the class it compiled at, lower after a fall-back
        S32 usedClass = 0;      ///< the class its file was found in
    };

    struct Options
    {
        /// The point variant: the vertex stage writes [[point_size]] from the extra
        /// uniform `point_size` in its default block. Only for point pipelines; Metal rejects a
        /// point size in the others.
        bool pointSize = false;
        /// A development knob: the vertex stage's position is invariant, as if its GLSL
        /// said `invariant gl_Position;` ([[position, invariant]]), which Metal keeps only with preserveInvariance.
        bool invariantPosition = false;
    };

    struct Program
    {
        std::string name;
        std::vector<Unit> units;             ///< in attach order
        std::vector<std::string> attributes; ///< the reserved attributes in binding order
        std::vector<std::string> blocks;     ///< the uniform blocks GL binds by name, in binding order
        S32 firstLevel = 0; ///< the program's shader level at its first link attempt; 0 when not known
        S32 finalLevel = 0; ///< the level it linked at, lower after a failed link
    };

    /// An input or output of a stage after the locations are set.
    struct Variable
    {
        std::string name;
        std::string type;  ///< as SPIR-V describes it, e.g. "float3"
        U32 location = 0;
        bool flat = false;
    };

    /// A member of a stage's default block as the MSL expects it in buffer 0.
    struct Uniform
    {
        std::string name;
        std::string type;
        U32 offset = 0;
        U32 size = 0;          ///< bytes, the whole array for an array
        U32 arraySize = 0;     ///< 0 when not an array
        U32 arrayStride = 0;
        U32 matrixStride = 0;  ///< 0 when not a matrix
        /// The stage's entry point reads it, as GL gives only active uniforms a location.
        bool active = false;
        /// For an array: its highest constant index read + 1, as Apple's GL reports the size; the
        /// declared size when an index is not a constant or the whole array is used. 0 when not an
        /// array or not read.
        U32 activeArraySize = 0;
    };

    /// Another uniform block, in buffer 1 + its GL binding.
    struct Block
    {
        std::string name;
        U32 size = 0;
        U32 buffer = 0;
    };

    /// A sampler, at the same index as texture and sampler.
    struct Texture
    {
        std::string name;
        std::string type; ///< 2d, cube, 2d_array, 3d; with "_shadow" for a depth comparison
        U32 index = 0;
    };

    struct Reflection
    {
        U32 defaultBlockSize = 0;       ///< bytes; 0 without loose uniforms
        std::vector<Uniform> uniforms;  ///< by offset
        std::vector<Block> blocks;
        std::vector<Texture> textures;  ///< by index
    };

    struct StageResult
    {
        Stage stage = Stage::Vertex;
        std::vector<U32> spirv;
        std::string msl;
        std::vector<Variable> inputs;
        std::vector<Variable> outputs;
        Reflection reflection;
        /// "file: compiled at class N, file from class M" for each unit of the stage.
        std::vector<std::string> classes;
    };

    struct Result
    {
        bool ok = false;
        bool glOnly = false;               ///< a geometry stage: only the GL backend has the program
        std::string error;                 ///< why not ok
        std::vector<std::string> warnings; ///< glslang's, on the allow-list
        std::vector<StageResult> stages;   ///< vertex, then fragment
    };

    /// The program in MSL, or why it cannot be. A unit that compiled at a lower shader class than
    /// it asked for fails it: Metal must not fall back where GL did.
    Result translate(const Program& program, const Options& options = Options());

    /// Translated programs on disk, so that GLSL to SPIR-V to MSL runs once. The key
    /// covers all a result depends on: the units' files and sources in attach order, the attributes
    /// and blocks, the options, what the caller passes as `capabilities` (the defines from the
    /// RHI's capabilities and the GPU family) and the versions of glslang, SPIRV-Cross and
    /// this translation. Only translations that succeeded are stored, without their SPIR-V.
    class DiskCache
    {
    public:
        explicit DiskCache(const std::string& folder);
        /// The cache of one generation (for the viewer, its build and the Mac's Metal GPU) in a
        /// folder of its own under `root`, named by the generation's digest. Every other folder and file under `root`
        /// is removed first: entries an earlier generation left, which no key of this one can meet (each key has the
        /// generation too), so the cache keeps one generation's translations at a time.
        DiskCache(const std::string& root, const std::string& generation);

        /// The folder the entries are in.
        const std::string& folder() const { return mFolder; }

        std::string key(const Program& program, const Options& options, const std::string& capabilities) const;
        bool load(const std::string& key, Result& result) const;
        bool store(const std::string& key, const Result& result) const;

        /// translate() through the cache; `hit` says whether it came from disk.
        Result translate(const Program& program, const Options& options, const std::string& capabilities,
                         bool* hit = nullptr) const;

    private:
        std::string mFolder;
    };
}

#endif // LL_LLRHIMSL_H
