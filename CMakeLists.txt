cmake_minimum_required(VERSION 3.21)
project(Wx264 LANGUAGES CXX)

find_package(Vulkan REQUIRED COMPONENTS glslc)

# Keep all executables under build/Release so the harness paths work unchanged
# regardless of which subtree defines a target.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
# CPU x264 reference tree, used only to generate parity fixtures and expected
# bytes. Never linked into the shipped encoder path at run time.
set(X264_REFERENCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/x264-vulkan")

include(CTest)
add_subdirectory(Wx264)

option(WX_BUILD_OBS "Build the Windows x64 OBS encoder plugin" OFF)
if(WX_BUILD_OBS)
    enable_language(C)
    if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "The OBS preview target requires a Windows x64 MSVC build")
    endif()
    add_subdirectory(obs)
endif()
