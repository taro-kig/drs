cmake_minimum_required(VERSION 3.20)
project(drs_ofx CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# static C++ runtime on Windows: no VC++ redistributable needed next to Resolve
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# OpenFX headers + C++ Support library (BSD-3), pinned by hash
include(FetchContent)
FetchContent_Declare(openfx
  URL https://github.com/AcademySoftwareFoundation/openfx/archive/refs/tags/OFX_Release_1.5.1.tar.gz
  URL_HASH SHA256=8C5766E2E0896B79FAB226A07C9EFFB52793F6BB14D0D30CF4631FDDB5F87E84
  SOURCE_SUBDIR _headers_only_ # do not add openfx's own CMake project
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(openfx)
set(OFX ${openfx_SOURCE_DIR})

add_library(ofxsupport STATIC
  ${OFX}/Support/Library/ofxsCore.cpp
  ${OFX}/Support/Library/ofxsImageEffect.cpp
  ${OFX}/Support/Library/ofxsInteract.cpp
  ${OFX}/Support/Library/ofxsLog.cpp
  ${OFX}/Support/Library/ofxsMultiThread.cpp
  ${OFX}/Support/Library/ofxsParams.cpp
  ${OFX}/Support/Library/ofxsProperty.cpp
  ${OFX}/Support/Library/ofxsPropertyValidation.cpp
)
target_include_directories(ofxsupport PUBLIC ${OFX}/include ${OFX}/Support/include ${OFX}/Support/Library)
set_target_properties(ofxsupport PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
  target_compile_definitions(ofxsupport PUBLIC NOMINMAX _CRT_SECURE_NO_WARNINGS)
  target_compile_options(ofxsupport PRIVATE /W0)
endif()

if(WIN32)
  set(OFX_ARCH Win64)
elseif(APPLE)
  set(OFX_ARCH MacOS)
else()
  set(OFX_ARCH Linux-x86-64)
endif()
set(BUNDLE_DIR ${CMAKE_BINARY_DIR}/bundles)

# one OFX bundle per plugin: <name>.ofx.bundle/Contents/<arch>/<name>.ofx
function(drs_ofx_plugin name)
  add_library(${name} MODULE ${ARGN})
  target_link_libraries(${name} PRIVATE ofxsupport)
  set_target_properties(${name} PROPERTIES PREFIX "" SUFFIX ".ofx" OUTPUT_NAME drs_${name})
  if(MSVC)
    target_compile_options(${name} PRIVATE /W4 /fp:fast)
  else()
    target_compile_options(${name} PRIVATE -Wall -O2)
    set_target_properties(${name} PROPERTIES CXX_VISIBILITY_PRESET hidden)
  endif()
  add_custom_command(TARGET ${name} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUNDLE_DIR}/drs_${name}.ofx.bundle/Contents/${OFX_ARCH}
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${name}> ${BUNDLE_DIR}/drs_${name}.ofx.bundle/Contents/${OFX_ARCH}/
  )
endfunction()

drs_ofx_plugin(ColorAnchors ColorAnchors/ColorAnchors.cpp)
drs_ofx_plugin(LookExplorer LookExplorer/LookExplorer.cpp)
target_include_directories(LookExplorer PRIVATE ColorAnchors) # shares ColorMath.h

# pixel math tests (no OFX host needed)
enable_testing()
add_executable(test_color_math tests/test_color_math.cpp)
target_include_directories(test_color_math PRIVATE ColorAnchors)
add_test(NAME color_math COMMAND test_color_math)
add_executable(test_look_math tests/test_look_math.cpp)
target_include_directories(test_look_math PRIVATE ColorAnchors LookExplorer)
add_test(NAME look_math COMMAND test_look_math)

# writes a BMP of the 9 starting looks on a synthetic chart, for eyeballing
add_executable(look_preview tools/look_preview.cpp)
target_include_directories(look_preview PRIVATE ColorAnchors LookExplorer)
