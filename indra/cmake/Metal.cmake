# -*- cmake -*-
# [Viewer Lab] The Metal backend's frameworks and its Objective-C++ files, Darwin only.
include_guard()

if (DARWIN)
  # Metal and QuartzCore, which AppKit loads already, and MetalFX weak-linked. The
  # Metal files build their code only for arm64 with a Metal 4 SDK (LL_RHI_METAL in llrhimetal.h).
  add_library( ll::metal INTERFACE IMPORTED )
  set_target_properties( ll::metal PROPERTIES INTERFACE_LINK_LIBRARIES
    "-framework Metal;-framework QuartzCore;-weak_framework MetalFX"
    )

  # Objective-C++ with ARC, without LL's precompiled header, as llwindow's .mm files.
  function(ll_metal_sources)
    set_source_files_properties(${ARGN} PROPERTIES
      COMPILE_OPTIONS "-fobjc-arc"
      SKIP_PRECOMPILE_HEADERS TRUE
      )
  endfunction()
endif ()
