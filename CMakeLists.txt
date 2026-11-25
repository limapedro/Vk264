cmake_minimum_required(VERSION 3.20)
project(vk264_phoenix VERSION 0.1.0 LANGUAGES C)

# Phoenix is a function-for-function CPU port of x264 (see reference/x264/).
# Every ported function lives in src/ with a unit test in tests/.
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/W3)
else()
    add_compile_options(-Wall -Wextra)
endif()

add_library(phx STATIC
    src/phx_param.c
    src/phx_tables.c
    src/phx_cost.c
    src/phx_frame.c
    src/phx_mbcache.c
    src/phx_bitstream.c
    src/phx_cabac.c
)
target_include_directories(phx PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

enable_testing()

add_executable(phx_test_param tests/test_param.c)
target_link_libraries(phx_test_param PRIVATE phx)
add_test(NAME phx_param_presets COMMAND phx_test_param)

add_executable(phx_test_cost tests/test_cost.c)
target_link_libraries(phx_test_cost PRIVATE phx)
add_test(NAME phx_cost_tables COMMAND phx_test_cost)

add_executable(phx_test_frame tests/test_frame.c)
target_link_libraries(phx_test_frame PRIVATE phx)
add_test(NAME phx_frame_lowres COMMAND phx_test_frame)

add_executable(phx_test_mbcache tests/test_mbcache.c)
target_link_libraries(phx_test_mbcache PRIVATE phx)
add_test(NAME phx_macroblock_cache COMMAND phx_test_mbcache)

add_executable(phx_test_bitstream tests/test_bitstream.c)
target_link_libraries(phx_test_bitstream PRIVATE phx)
add_test(NAME phx_bitstream COMMAND phx_test_bitstream)

add_executable(phx_test_cabac tests/test_cabac.c)
target_link_libraries(phx_test_cabac PRIVATE phx)
add_test(NAME phx_cabac COMMAND phx_test_cabac)
