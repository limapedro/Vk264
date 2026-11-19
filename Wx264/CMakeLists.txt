cmake_minimum_required(VERSION 3.21)
project(Wx264Gpu LANGUAGES CXX)

find_package(Vulkan REQUIRED COMPONENTS glslc)

set(shader_dir "${CMAKE_CURRENT_BINARY_DIR}/shaders")
foreach(shader IN ITEMS pyramid halfpel motion spatial older_references residual flat_intra deblock deblock_frame mb_flags mb_entropy entropy scan assemble)
    set(output "${shader_dir}/${shader}.spv")
    add_custom_command(OUTPUT "${output}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${shader_dir}"
        COMMAND Vulkan::glslc --target-env=vulkan1.1 -O
                "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}.comp" -o "${output}"
        DEPENDS "shaders/${shader}.comp" "shaders/layout.glsl"
                "shaders/transform.glsl" "shaders/bitstream.glsl" "shaders/deblock_tables.glsl"
                "shaders/macroblock_syntax.glsl" "shaders/deblock_filter.glsl"
                "vulkan/cavlc_residual.glsl"
        VERBATIM)
    list(APPEND shaders "${output}")
endforeach()

add_custom_target(wx264_shaders DEPENDS ${shaders})
add_executable(wx264 src/main.cpp src/gpu.cpp)
add_dependencies(wx264 wx264_shaders)
target_link_libraries(wx264 PRIVATE Vulkan::Vulkan)
target_compile_features(wx264 PRIVATE cxx_std_17)
target_compile_definitions(wx264 PRIVATE WX_SHADER_DIR="${shader_dir}")
if(MSVC)
    target_compile_options(wx264 PRIVATE /W4)
else()
    target_compile_options(wx264 PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_subdirectory(vulkan)
target_link_libraries(wx264 PRIVATE x264_vulkan_backend)

if(BUILD_TESTING)
    find_package(Python3 COMPONENTS Interpreter)
    find_program(FFMPEG ffmpeg)
    find_program(FFPROBE ffprobe)
    if(Python3_FOUND AND FFMPEG AND FFPROBE)
        add_test(NAME shader_reconstruction
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../harness/check.py"
                    --encoder $<TARGET_FILE:wx264> --ffmpeg "${FFMPEG}" --ffprobe "${FFPROBE}")
        add_test(NAME vulkan_encoder_reconstruction
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../harness/check.py"
                    --encoder $<TARGET_FILE:wx264> --ffmpeg "${FFMPEG}" --ffprobe "${FFPROBE}" --vulkan)
        add_test(NAME vulkan_cavlc_reconstruction
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../harness/check.py"
                    --encoder $<TARGET_FILE:wx264> --ffmpeg "${FFMPEG}" --ffprobe "${FFPROBE}" --vulkan --entropy cavlc)
    endif()
endif()
