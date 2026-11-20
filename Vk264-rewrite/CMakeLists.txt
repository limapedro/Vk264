cmake_minimum_required(VERSION 3.21)
project(vk264_rewrite LANGUAGES C)

# Correctness-first C + Vulkan port of x264. No C++ is used anywhere in this
# tree: the host side is C11 and the device side is GLSL compute compiled to
# SPIR-V. The reference x264 tree is never linked into the encoder.
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

find_package(Vulkan REQUIRED COMPONENTS glslc)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(X264_REF "${CMAKE_CURRENT_SOURCE_DIR}/../references/x264-master")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# ---- GLSL compute shaders -------------------------------------------------
file(GLOB REWRITE_SHADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.comp")
set(SPIRV "")
foreach(shader IN LISTS REWRITE_SHADERS)
    get_filename_component(name "${shader}" NAME_WE)
    set(spv "${CMAKE_CURRENT_BINARY_DIR}/shaders/${name}.spv")
    add_custom_command(OUTPUT "${spv}"
        COMMAND Vulkan::glslc --target-env=vulkan1.1 -O "${shader}" -o "${spv}"
        DEPENDS "${shader}"
        COMMENT "glslc ${name}.comp"
        VERBATIM)
    list(APPEND SPIRV "${spv}")
endforeach()
add_custom_target(vk264_rewrite_shaders DEPENDS ${SPIRV})

# ---- Encoder library (C only) ---------------------------------------------
# The reference C port is a byte-exact oracle linked into tests only.
file(GLOB REWRITE_REF CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/ref/*.c")
add_library(vk264_rewrite_ref STATIC ${REWRITE_REF})
target_include_directories(vk264_rewrite_ref PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src/ref")

# Thin Vulkan host driver. No encoding algorithms live here.
file(GLOB REWRITE_HOST CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/host/*.c")
add_library(vk264_rewrite_host STATIC ${REWRITE_HOST})
target_include_directories(vk264_rewrite_host PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src/host")
target_link_libraries(vk264_rewrite_host PUBLIC Vulkan::Vulkan)

add_executable(vk264_rewrite "${CMAKE_CURRENT_SOURCE_DIR}/src/main.c")
target_link_libraries(vk264_rewrite PRIVATE vk264_rewrite_host)
add_dependencies(vk264_rewrite vk264_rewrite_shaders)

# Shader directory is passed at compile time so the binary can find its SPIR-V.
target_compile_definitions(vk264_rewrite PRIVATE
    VK264_SHADER_DIR="${CMAKE_CURRENT_BINARY_DIR}/shaders/")

include(CTest)
add_subdirectory(tests)
