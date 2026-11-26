# FullImmersion: a 3D desktop shared with Milk's agents. S0 is the spikes: capture, quad layers, input.
cmake_minimum_required(VERSION 3.25)
project(FullImmersion LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "FullImmersion's spikes use Windows.Graphics.Capture and Direct3D 11: Windows only for now.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
add_compile_options(/W4 /permissive- /utf-8 /EHsc /Zc:__cplusplus)

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# OpenXR: headers and the loader. The runtime (SteamVR) is found at run time through the registry.
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
set(BUILD_CONFORMANCE_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(OpenXR
  URL https://github.com/KhronosGroup/OpenXR-SDK/archive/refs/tags/release-1.1.43.tar.gz)
FetchContent_MakeAvailable(OpenXR)

# fi_common: window listing, capture (WGC), input into real windows, D3D11 helpers. No OpenXR here.
add_library(fi_common STATIC
  src/common/desktop.cpp
  src/common/capture.cpp
  src/common/input.cpp)
target_include_directories(fi_common PUBLIC src)
target_link_libraries(fi_common PUBLIC d3d11 dxgi dwmapi windowsapp)

add_executable(s1_capture spikes/s1_capture/main.cpp)
target_link_libraries(s1_capture PRIVATE fi_common)

add_executable(s2_quad spikes/s2_quad/main.cpp)
target_link_libraries(s2_quad PRIVATE fi_common openxr_loader d3dcompiler)

add_executable(s3_input spikes/s3_input/main.cpp)
target_link_libraries(s3_input PRIVATE fi_common)
