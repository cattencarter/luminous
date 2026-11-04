# -*- cmake -*-
# [Viewer Lab] SPIRV-Cross, vendored in indra/third_party, Darwin only: SPIR-V to MSL and
# reflection for the Metal backend's shader translation.

add_library( ll::spirv_cross INTERFACE IMPORTED )

if (DARWIN)
  # As in Glslang.cmake: no usage requirements, system headers.
  target_link_libraries( ll::spirv_cross INTERFACE
    $<LINK_ONLY:spirv-cross-msl>
    $<LINK_ONLY:spirv-cross-glsl>
    $<LINK_ONLY:spirv-cross-reflect>
    $<LINK_ONLY:spirv-cross-core>
    )
  target_include_directories( ll::spirv_cross SYSTEM INTERFACE ${CMAKE_SOURCE_DIR}/third_party/spirv-cross )
endif ()
