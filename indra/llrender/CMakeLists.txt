# -*- cmake -*-

project(llrender)

include(00-Common)
include(OpenGL)
include(FreeType)
include(LLCommon)
include(LLImage)
include(LLWindow)

set(llrender_SOURCE_FILES
    llcubemap.cpp
    llcubemaparray.cpp
    llfontbitmapcache.cpp
    llfontfreetype.cpp
    llfontfreetypesvg.cpp
    llfontgl.cpp
    llfontvertexbuffer.cpp
    llfontregistry.cpp
    llgl.cpp
    llglslshader.cpp
    llgltexture.cpp
    llimagegl.cpp
    llpostprocess.cpp
    llrender.cpp
    llrender2dutils.cpp
    llrendernavprim.cpp
    llrendersphere.cpp
    llrendertarget.cpp
    llnativeprogram.cpp
    llrhi.cpp
    llrhigl.cpp
    llshadermgr.cpp
    lltexture.cpp
    lltexturemanagerbridge.cpp
    lluiimage.cpp
    llvertexbuffer.cpp
    llglcommonfunc.cpp
    )
    
set(llrender_HEADER_FILES
    CMakeLists.txt

    llcubemap.h
    llcubemaparray.h
    llfontgl.h
    llfontvertexbuffer.h
    llfontfreetype.h
    llfontfreetypesvg.h
    llfontbitmapcache.h
    llfontregistry.h
    llgl.h
    llglheaders.h
    llglslshader.h
    llglstates.h
    llgltexture.h
    llgltypes.h
    llimagegl.h
    llpostprocess.h
    llrender.h
    llrender2dutils.h
    llrendernavprim.h
    llrendersphere.h
    llmslinclude.h
    llnativeprogram.h
    llrhi.h
    llrhigl.h
    llshadermgr.h
    lltexture.h
    lltexturemanagerbridge.h
    lluiimage.h
    lluiimage.inl
    llvertexbuffer.h
    llglcommonfunc.h
    )

list(APPEND llrender_SOURCE_FILES ${llrender_HEADER_FILES})

if (BUILD_HEADLESS)
  add_library (llrenderheadless
    ${llrender_SOURCE_FILES}
    )

  set_property(TARGET llrenderheadless
    PROPERTY COMPILE_DEFINITIONS LL_MESA=1 LL_MESA_HEADLESS=1
      )

  target_link_libraries(llrenderheadless
          llcommon
          llimage
          llmath
          llrender
          llxml
          llfilesystem
          )
endif (BUILD_HEADLESS)

# [Viewer Lab] The translation to MSL, Darwin only, with the shader tool chain.
if (DARWIN)
  include(Glslang)
  include(SPIRVCross)
  include(Metal)
  list(APPEND llrender_SOURCE_FILES llrhimsl.cpp llrhimetaldevice-objc.mm llrhimetalresource-objc.mm
       llrhimetalencoder-objc.mm llrhimetalpipeline-objc.mm llrhimetalsurface-objc.mm
       llrhimetalbinding-objc.mm llrhimetalnative-objc.mm)
  list(APPEND llrender_HEADER_FILES llrhimsl.h llrhimetal.h llrhimetal-objc.h)
  ll_metal_sources(llrhimetaldevice-objc.mm llrhimetalresource-objc.mm llrhimetalencoder-objc.mm
                   llrhimetalpipeline-objc.mm llrhimetalsurface-objc.mm
                   llrhimetalbinding-objc.mm llrhimetalnative-objc.mm) # the Metal backend
endif ()

add_library (llrender ${llrender_SOURCE_FILES})
target_include_directories( llrender  INTERFACE   ${CMAKE_CURRENT_SOURCE_DIR})

set_property(TARGET llrender PROPERTY POSITION_INDEPENDENT_CODE ON)

# Libraries on which this library depends, needed for Linux builds
# Sort by high-level to low-level
target_link_libraries(llrender
        llcommon
        llimage
        llmath
        llfilesystem
        llxml
        llwindow
        ll::freetype
        OpenGL::GL
        OpenGL::GLU
        )

if (DARWIN)
  target_link_libraries(llrender ll::glslang ll::spirv_cross ll::metal) # [Viewer Lab]
endif ()

# Add tests
if (LL_TESTS)
  include(LLAddBuildTest)
  # UNIT TESTS
  set(llrender_TEST_SOURCE_FILES
    llrhi.cpp
    )
  # The GL backend's tests make their own GL context with CGL, so they run on macOS.
  if (DARWIN)
    list(APPEND llrender_TEST_SOURCE_FILES llrhigl.cpp llrhimsl.cpp)
    set_property(SOURCE llrhigl.cpp PROPERTY LL_TEST_ADDITIONAL_LIBRARIES llrender llimage)
    set_property(SOURCE llrhimsl.cpp PROPERTY LL_TEST_ADDITIONAL_SOURCE_FILES tests/llshadertoolchain_metal.mm)
    set_property(SOURCE llrhimsl.cpp PROPERTY LL_TEST_ADDITIONAL_LIBRARIES
      ll::glslang ll::spirv_cross "-framework Metal" "-framework Foundation")
    # Objective-C++ with ARC, without LL's precompiled header, as llwindow's .mm files.
    set_source_files_properties(tests/llshadertoolchain_metal.mm PROPERTIES
      COMPILE_OPTIONS "-fobjc-arc"
      SKIP_PRECOMPILE_HEADERS TRUE
      )
  endif ()
  LL_ADD_PROJECT_UNIT_TESTS(llrender "${llrender_TEST_SOURCE_FILES}")
  # [Viewer Lab] The shader tool chain's libraries link from here; Darwin only.
  if (DARWIN)
    LL_ADD_INTEGRATION_TEST(llshadertoolchain "tests/llshadertoolchain_metal.mm"
      "ll::glslang;ll::spirv_cross;llcommon;-framework Metal;-framework Foundation")
    # The Metal backend on this Mac, under Metal API Validation and shader validation.
    # An API Validation warning aborts the test program as an error does. A shader validation
    # report only goes to stderr, so each test runs with stderr in a file and fails on a
    # validation line there (runValidated in tests/llrhimetal_metal.mm).
    # run_build_test.py sets the variables; the test program comes last on its own, without {}.
    # The device's tests read Metal's objects, so they are Objective-C++.
    ll_metal_sources(tests/llrhimetal_metal.mm)
    LL_ADD_INTEGRATION_TEST(llrhimetal "tests/llrhimetal_metal.mm" "llrender;llimage;llcommon;ll::metal;-framework Foundation"
      -DMTL_DEBUG_LAYER=1 -DMTL_DEBUG_LAYER_ERROR_MODE=assert -DMTL_DEBUG_LAYER_WARNING_MODE=assert
      -DMTL_SHADER_VALIDATION=1 -DMTL_SHADER_VALIDATION_REPORT_TO_STDERR=1
      -DMTL_DEBUG_LAYER_VALIDATE_STORE_ACTIONS=1 -DLL_METAL_DEBUG=1)
    # The Metal debug setting turns validation on by itself, without the variables above.
    ll_metal_sources(tests/llrhimetaldebug_metal.mm)
    LL_ADD_INTEGRATION_TEST(llrhimetaldebug "tests/llrhimetaldebug_metal.mm" "llrender;llcommon;ll::metal;-framework Foundation")
  endif ()
endif (LL_TESTS)
