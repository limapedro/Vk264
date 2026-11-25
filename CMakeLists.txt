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
    src/phx_headers.c
    src/phx_encoder.c
    src/phx_slice.c
    src/phx_cabac_syntax.c
    src/phx_predict.c
    src/phx_dct.c
    src/phx_quant.c
    src/phx_cavlc.c
    src/phx_mbencode.c
    src/phx_analysis.c
    src/phx_lookahead.c
    src/phx_sei.c
    src/phx_chroma.c
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

add_executable(phx_header_dump tools/header_dump.c)
target_link_libraries(phx_header_dump PRIVATE phx)

add_executable(phx_cli tools/phx_cli.c)
target_link_libraries(phx_cli PRIVATE phx)

add_executable(phx_test_encoder tests/test_phx_encoder.c)
target_link_libraries(phx_test_encoder PRIVATE phx)
add_test(NAME phx_encoder COMMAND phx_test_encoder)

add_executable(phx_test_slice tests/test_phx_slice.c)
target_link_libraries(phx_test_slice PRIVATE phx)
add_test(NAME phx_slice_header COMMAND phx_test_slice)

add_executable(phx_test_cabac_syntax tests/test_phx_cabac_syntax.c)
target_link_libraries(phx_test_cabac_syntax PRIVATE phx)
add_test(NAME phx_cabac_syntax COMMAND phx_test_cabac_syntax)

add_executable(phx_test_predict tests/test_phx_predict.c)
target_link_libraries(phx_test_predict PRIVATE phx)
add_test(NAME phx_predict COMMAND phx_test_predict)

add_executable(phx_test_quant tests/test_phx_quant.c)
target_link_libraries(phx_test_quant PRIVATE phx)
add_test(NAME phx_quant_dequant COMMAND phx_test_quant)

add_executable(phx_test_cavlc tests/test_phx_cavlc.c)
target_link_libraries(phx_test_cavlc PRIVATE phx)
add_test(NAME phx_cavlc COMMAND phx_test_cavlc)

add_executable(phx_test_mbencode tests/test_phx_mbencode.c)
target_link_libraries(phx_test_mbencode PRIVATE phx)
add_test(NAME phx_mbencode COMMAND phx_test_mbencode)

add_executable(phx_test_analysis tests/test_phx_analysis.c)
target_link_libraries(phx_test_analysis PRIVATE phx)
add_test(NAME phx_analysis COMMAND phx_test_analysis)

add_executable(phx_test_lookahead tests/test_phx_lookahead.c)
target_link_libraries(phx_test_lookahead PRIVATE phx)
add_test(NAME phx_lookahead COMMAND phx_test_lookahead)

add_executable(phx_test_sei tests/test_phx_sei.c)
target_link_libraries(phx_test_sei PRIVATE phx)
add_test(NAME phx_sei COMMAND phx_test_sei)

add_executable(phx_test_chroma tests/test_phx_chroma.c)
target_link_libraries(phx_test_chroma PRIVATE phx)
add_test(NAME phx_chroma COMMAND phx_test_chroma)
