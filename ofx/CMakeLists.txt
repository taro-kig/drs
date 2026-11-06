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

# stb_truetype (public domain) for Photo Frame captions, pinned by commit and hash
FetchContent_Declare(stb_truetype
  URL https://raw.githubusercontent.com/nothings/stb/d14d46f6b44456eb88beac58cba833a4135d4c89/stb_truetype.h
  URL_HASH SHA256=ECD30B05E0DD4FEA3A13C26810DD9E1992DC379049482C393D5A19E6B5090AAB
  DOWNLOAD_NO_EXTRACT TRUE
)
FetchContent_MakeAvailable(stb_truetype)
set(STB_DIR ${stb_truetype_SOURCE_DIR})

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
  add_library(${name} MODULE ${ARGN} ${CMAKE_CURRENT_SOURCE_DIR}/common/ClLut.cpp)
  target_link_libraries(${name} PRIVATE ofxsupport ${CMAKE_DL_LIBS})
  # common/ = LUT + OpenCL helpers, ColorAnchors/ = ColorMath.h
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/common ${CMAKE_CURRENT_SOURCE_DIR}/ColorAnchors)
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
drs_ofx_plugin(ColorLook ColorLook/ColorLook.cpp)
target_include_directories(ColorLook PRIVATE LookExplorer)
drs_ofx_plugin(HueVectors HueVectors/HueVectors.cpp)
drs_ofx_plugin(PhotoFrame PhotoFrame/PhotoFrame.cpp PhotoFrame/TextRaster.cpp)
target_include_directories(PhotoFrame PRIVATE PhotoFrame ${STB_DIR})
target_include_directories(HueVectors PRIVATE HueVectors)

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
target_include_directories(look_preview PRIVATE ColorAnchors LookExplorer ColorLook)

add_executable(test_lut tests/test_lut.cpp)
target_include_directories(test_lut PRIVATE common ColorAnchors LookExplorer HueVectors ColorLook)
add_test(NAME lut COMMAND test_lut)

# runs the OpenCL kernel on this machine's GPU (skips without OpenCL)
add_executable(test_cl tests/test_cl.cpp common/ClLut.cpp)
target_include_directories(test_cl PRIVATE common ColorAnchors LookExplorer ColorLook PhotoFrame)
target_link_libraries(test_cl PRIVATE ${CMAKE_DL_LIBS})
add_test(NAME opencl COMMAND test_cl)

add_executable(test_mixer tests/test_mixer.cpp)
target_include_directories(test_mixer PRIVATE common ColorAnchors ColorLook)
add_test(NAME mixer COMMAND test_mixer)

add_executable(test_color_look tests/test_color_look.cpp)
target_include_directories(test_color_look PRIVATE common ColorAnchors LookExplorer ColorLook)
add_test(NAME color_look COMMAND test_color_look)

add_executable(test_hero tests/test_hero.cpp)
target_include_directories(test_hero PRIVATE common ColorAnchors LookExplorer ColorLook)
add_test(NAME hero COMMAND test_hero)

add_executable(test_huevec tests/test_huevec.cpp)
target_include_directories(test_huevec PRIVATE common ColorAnchors HueVectors)
add_test(NAME huevec COMMAND test_huevec)

add_executable(test_frame tests/test_frame.cpp PhotoFrame/TextRaster.cpp)
target_include_directories(test_frame PRIVATE common ColorAnchors PhotoFrame ${STB_DIR})
add_test(NAME frame COMMAND test_frame)
