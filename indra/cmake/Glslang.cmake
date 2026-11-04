# -*- cmake -*-
# [Viewer Lab] glslang, vendored in indra/third_party, Darwin only: GLSL to SPIR-V for
# the Metal backend's shader translation.

add_library( ll::glslang INTERFACE IMPORTED )

if (DARWIN)
  # The libraries without their usage requirements, and their headers as system headers, so the
  # viewer's -Werror never applies to warnings in them.
  target_link_libraries( ll::glslang INTERFACE
    $<LINK_ONLY:glslang>
    $<LINK_ONLY:glslang-default-resource-limits>
    )
  target_include_directories( ll::glslang SYSTEM INTERFACE ${CMAKE_SOURCE_DIR}/third_party/glslang )
endif ()
