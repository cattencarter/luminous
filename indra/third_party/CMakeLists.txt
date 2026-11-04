# -*- cmake -*-
# [Viewer Lab] glslang and SPIRV-Cross, vendored as released, for the Metal backend's shader
# translation (P1-04a, README.md here). Built on Darwin only, and only for targets that link them
# through ll::glslang and ll::spirv_cross (indra/cmake/Glslang.cmake, SPIRVCross.cmake).

# The viewer's compile options (00-Common.cmake) are included per library directory, so none
# reach this one: the code builds with its own warnings, and with symbols for the viewer's.
add_compile_options(-g)
set(BUILD_SHARED_LIBS OFF)

# glslang: the library and its command-line tool, without HLSL, the optimizer (SPIRV-Tools),
# the external dependencies, tests or install rules.
set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
set(ENABLE_GLSLANG_BINARIES ON CACHE BOOL "" FORCE)
set(ENABLE_GLSLANG_JS OFF CACHE BOOL "" FORCE)
add_subdirectory(glslang)

# SPIRV-Cross: static libraries and the command-line tool, without tests or install rules.
set(SPIRV_CROSS_STATIC ON CACHE BOOL "" FORCE)
set(SPIRV_CROSS_SHARED OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_CLI ON CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_SKIP_INSTALL ON CACHE BOOL "" FORCE)
add_subdirectory(spirv-cross)
