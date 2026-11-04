# Vendored third-party code

[Viewer Lab] glslang and SPIRV-Cross, for the Metal backend's shader translation (GLSL to
SPIR-V to MSL, and reflection). Chosen by the project owner on 2026-10-16 (task P1-04a) over
private autobuild packages, so the build needs no new repositories, tokens or downloads.

They build on Darwin only, and only for the targets that link them through `ll::glslang`
(`indra/cmake/Glslang.cmake`) and `ll::spirv_cross` (`indra/cmake/SPIRVCross.cmake`). Those
link the libraries without their usage requirements and give their headers as system headers,
so the viewer's `-Werror` never applies to warnings in them. The viewer links both on Darwin
arm64 and registers the translator only on the Metal path (P3-01f); the GL path never calls them.
`CMakeLists.txt` here sets their options; the viewer's compile options (`00-Common.cmake`) are
included per library directory and never reach this one, so the code builds with its own
warnings, plus `-g`. pre-commit skips the two library directories (`.pre-commit-config.yaml`).
glslang needs CMake 3.22.1 or newer, so configuring on Darwin does too.

| Library | Version | Commit | Archive | SHA-256 of the archive |
| --- | --- | --- | --- | --- |
| glslang | 16.6.0 (2026-09-11) | `e1b562a8bed273a02f30b59b66a5d499793cede5` | https://github.com/KhronosGroup/glslang/archive/refs/tags/16.6.0.tar.gz | `9c09b901149c729df745057dafa815278aaa101b84d2b6e14f16a42de52f97f2` |
| SPIRV-Cross | vulkan-sdk-1.4.363.0 (2026-09-16) | `f11ba9f0b21ba8fc15153d50a2a1ae31ab1cf8f7` | https://github.com/KhronosGroup/SPIRV-Cross/archive/refs/tags/vulkan-sdk-1.4.363.0.tar.gz | `f708982e88b763ef5b0394ed468fc2c3628f68cdab89f2e7a946cd628c04e721` |

The commits are the ones the archives name (their pax header). Every file here is as in the
archive, byte for byte apart from line endings: git stores and checks out text with LF
(`.gitattributes`), and `glslang/gen_extension_headers.py` has CRLF in the archive. Left out:

- glslang: `Test/`, `gtests/`, `External/`, `kokoro/`, `ndk_test/`, `gn_scripts/`,
  `build_overrides/`, `.github/`, `Android.mk`, `BUILD.gn`, `.gn`, `DEPS`, `known_good*.json`,
  `standalone.gclient`, `update_glslang_sources.py`, `build_info.py`, `_config.yml`,
  `license-checker.cfg`, `.clang-format`, `.gitattributes`, `.gitignore`, `.mailmap`, and
  `CODE_OF_CONDUCT.md`, `CONTRIBUTING.md`, `SECURITY.md`.
- SPIRV-Cross: `reference/`, `shaders*/`, `tests-other/`, `samples/`, `gn/`, `.github/`,
  `Makefile`, `Package.swift`, `appveyor.yml`, the test and format scripts, `spirv_cross.natvis`,
  `.clang-format`, `.gitignore` and `CODE_OF_CONDUCT.adoc`.

## Licenses

Per file, as each project's license map gives them (`glslang/REUSE.toml`,
`spirv-cross/.reuse/dep5`); the texts are in each `LICENSES/` directory.

glslang (`glslang/LICENSE.txt`):

- BSD-3-Clause: most of the code.
- BSD-2-Clause: the resource limits' C part and the C interfaces
  (`glslang/ResourceLimits/resource_limits_c.cpp`, `glslang/Public/resource_limits_c.h`,
  `glslang/CInterface/`, `glslang/Include/glslang_c_*.h`, `SPIRV/CInterface/`).
- AML-glslang (NVIDIA, 2002): the preprocessor (`glslang/MachineIndependent/preprocessor/Pp*`).
  Redistributions must keep NVIDIA's copyright notice, the license text and its disclaimers.
- MIT-Khronos-old: the SPIR-V headers (`SPIRV/GLSL.ext.*.h`, `SPIRV/GLSL.std.450.h`,
  `SPIRV/NonSemantic*.h`, `SPIRV/spirv.hpp11`) and `glslang/MachineIndependent/gl_types.h`.
- Apache-2.0: `SPIRV/bitutils.h`, `SPIRV/hex_float.h`, and build scripts (`parse_version.cmake`,
  `gen_extension_headers.py`), Markdown and `LICENSE.txt`.
- GPL-3.0-or-later WITH Bison-exception-2.2: the Bison-generated parser
  (`glslang/MachineIndependent/glslang_tab.cpp` and `.h`); the exception lets it be used in other
  programs on their own terms.

SPIRV-Cross (`spirv-cross/LICENSE`):

- Apache-2.0 OR MIT: the code.
- LicenseRef-KhronosFreeUse (MIT-style, Khronos): `spirv.h`, `spirv.hpp`, `GLSL.std.450.h`,
  `NonSemanticShaderDebugInfo100.h`.
- CC-BY-4.0: the documentation.

When the libraries are linked into the viewer (P1-04c), the viewer's packaging must ship these
notices with it.

## Tools and updates

The command-line tools build on request: the Xcode targets `glslang-standalone` (the binary
`glslang`) and `spirv-cross`. SPIRV-Cross's CMake runs `git describe` in its source directory, so
`spirv-cross --revision` shows this repository's commit, not SPIRV-Cross's; the version is the
one above.

To update: download the new archives, check them, replace the two directories with the same
files left out, compare every file with the archive, and change the table above, the licenses if
the maps changed, and the versions in `CLAUDE.md` (Værktøjer).
