add_executable(vk264_oracle_cli oracle_cli.c)
target_link_libraries(vk264_oracle_cli PRIVATE vk264_rewrite_ref)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
# The x264 reference tree is READ-ONLY: never build binaries there. Use its
# binary if present, otherwise the scratch build under build/x264-ref-src/.
set(VK264_X264_EXE "${CMAKE_CURRENT_SOURCE_DIR}/../../references/x264-master/x264.exe")
if(NOT EXISTS "${VK264_X264_EXE}")
  set(VK264_X264_EXE "${CMAKE_BINARY_DIR}/../../build/x264-ref-src/x264.exe")
endif()
add_test(NAME rewrite_oracle_flat_sweep
    COMMAND "${CMAKE_COMMAND}" -E env "VK264_X264_EXE=${VK264_X264_EXE}"
            "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_flat_sweep.py"
            "$<TARGET_FILE:vk264_oracle_cli>" "${CMAKE_BINARY_DIR}")
add_test(NAME rewrite_oracle_real_crops
    COMMAND "${CMAKE_COMMAND}" -E env "VK264_X264_EXE=${VK264_X264_EXE}"
            "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_real_crops.py"
            "$<TARGET_FILE:vk264_oracle_cli>" "${CMAKE_BINARY_DIR}")
add_test(NAME rewrite_oracle_multimb
    COMMAND "${CMAKE_COMMAND}" -E env "VK264_X264_EXE=${VK264_X264_EXE}"
            "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_multimb.py"
            "$<TARGET_FILE:vk264_oracle_cli>" "${CMAKE_BINARY_DIR}")
add_test(NAME rewrite_oracle_pskip
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_pskip.py"
            "${VK264_X264_EXE}" "$<TARGET_FILE:vk264_oracle_pcli>" "${CMAKE_BINARY_DIR}")
add_test(NAME rewrite_oracle_pme
    COMMAND "${CMAKE_COMMAND}" -E env "VK264_X264_EXE=${VK264_X264_EXE}"
            "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_pme.py"
            "$<TARGET_FILE:vk264_oracle_pme_cli>" "${CMAKE_BINARY_DIR}")

add_executable(vk264_hpel_test test_hpel.c)
target_link_libraries(vk264_hpel_test PRIVATE vk264_rewrite_ref)
add_test(NAME rewrite_hpel_filter COMMAND vk264_hpel_test)

add_executable(vk264_subpel_test test_subpel.c)
target_link_libraries(vk264_subpel_test PRIVATE vk264_rewrite_ref)
add_test(NAME rewrite_subpel_me COMMAND vk264_subpel_test)

add_executable(vk264_i4x4_test test_i4x4.c)
target_link_libraries(vk264_i4x4_test PRIVATE vk264_rewrite_ref)
add_test(NAME rewrite_i4x4_decide COMMAND vk264_i4x4_test)

add_executable(vk264_i4x4enc_test test_i4x4enc.c)
target_link_libraries(vk264_i4x4enc_test PRIVATE vk264_rewrite_ref)
add_test(NAME rewrite_i4x4_emit COMMAND vk264_i4x4enc_test)

add_executable(vk264_encoder_test test_encoder.c)
target_link_libraries(vk264_encoder_test PRIVATE vk264_rewrite_ref)
add_test(NAME rewrite_encoder_orchestration COMMAND vk264_encoder_test)

add_executable(vk264_ratecontrol_test test_ratecontrol.c)
target_link_libraries(vk264_ratecontrol_test PRIVATE vk264_rewrite_ref)
add_test(NAME rewrite_ratecontrol_primitives COMMAND vk264_ratecontrol_test)

add_executable(vk264_slicetype_test test_slicetype.c)
target_link_libraries(vk264_slicetype_test PRIVATE vk264_rewrite_ref)
add_test(NAME rewrite_slicetype_decide COMMAND vk264_slicetype_test)

add_executable(vk264_i8x8_test test_i8x8.c)
target_link_libraries(vk264_i8x8_test PRIVATE vk264_rewrite_ref)
add_test(NAME rewrite_i8x8_primitives COMMAND vk264_i8x8_test)

add_executable(vk264_i4mb0 test_i4mb0.c)
target_link_libraries(vk264_i4mb0 PRIVATE vk264_rewrite_ref)

add_executable(vk264_dump_i4mb0 dump_i4mb0.c)
target_link_libraries(vk264_dump_i4mb0 PRIVATE vk264_rewrite_ref)

add_executable(vk264_oracle_pcli oracle_pcli.c)
target_link_libraries(vk264_oracle_pcli PRIVATE vk264_rewrite_ref)

add_executable(vk264_oracle_pme_cli oracle_pme_cli.c)
target_link_libraries(vk264_oracle_pme_cli PRIVATE vk264_rewrite_ref)

add_executable(vk264_oracle_video_cli oracle_video_cli.c)
target_link_libraries(vk264_oracle_video_cli PRIVATE vk264_rewrite_ref)

add_executable(vk264_oracle_cabac_cli oracle_cabac_cli.c)
target_link_libraries(vk264_oracle_cabac_cli PRIVATE vk264_rewrite_ref)
add_test(NAME rewrite_cabac
    COMMAND "${CMAKE_COMMAND}" -E env "VK264_X264_EXE=${VK264_X264_EXE}"
            "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_cabac.py"
            "$<TARGET_FILE:vk264_oracle_cabac_cli>" "${CMAKE_BINARY_DIR}")
add_test(NAME rewrite_cabac_pskip
    COMMAND "${CMAKE_COMMAND}" -E env "VK264_X264_EXE=${VK264_X264_EXE}"
            "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_cabac_pskip.py"
            "$<TARGET_FILE:vk264_oracle_cabac_cli>" "${CMAKE_BINARY_DIR}")
add_test(NAME rewrite_cabac_pme
    COMMAND "${CMAKE_COMMAND}" -E env "VK264_X264_EXE=${VK264_X264_EXE}"
            "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_cabac_pme.py"
            "$<TARGET_FILE:vk264_oracle_cabac_cli>" "${CMAKE_BINARY_DIR}")
add_test(NAME rewrite_cabac_textured
    COMMAND "${CMAKE_COMMAND}" -E env "VK264_X264_EXE=${VK264_X264_EXE}"
            "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_cabac_textured.py"
            "$<TARGET_FILE:vk264_oracle_cabac_cli>" "${CMAKE_BINARY_DIR}")
add_test(NAME rewrite_cabac_intra_p
    COMMAND "${CMAKE_COMMAND}" -E env "VK264_X264_EXE=${VK264_X264_EXE}"
            "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_cabac_intra_p.py"
            "$<TARGET_FILE:vk264_oracle_cabac_cli>" "${CMAKE_BINARY_DIR}")

add_executable(vk264_test_bitstream test_bitstream.c)
target_link_libraries(vk264_test_bitstream PRIVATE vk264_rewrite_ref)
add_test(NAME rewrite_bitstream COMMAND vk264_test_bitstream)

add_executable(vk264_test_motion test_motion.c)
target_link_libraries(vk264_test_motion PRIVATE vk264_rewrite_ref)
add_test(NAME rewrite_motion COMMAND vk264_test_motion)

add_executable(vk264_test_nal test_nal.c)
target_link_libraries(vk264_test_nal PRIVATE vk264_rewrite_ref)
add_test(NAME rewrite_nal COMMAND vk264_test_nal)

add_executable(vk264_test_slice test_slice.c)
target_link_libraries(vk264_test_slice PRIVATE vk264_rewrite_ref)
add_test(NAME rewrite_slice COMMAND vk264_test_slice)

add_executable(vk264_test_shader_dct test_shader_dct.c)
target_link_libraries(vk264_test_shader_dct PRIVATE vk264_rewrite_ref vk264_rewrite_host)
target_compile_definitions(vk264_test_shader_dct PRIVATE
    VK264_SHADER_DIR="${CMAKE_BINARY_DIR}/shaders/")
add_dependencies(vk264_test_shader_dct vk264_rewrite_shaders)
add_test(NAME rewrite_shader_dct COMMAND vk264_test_shader_dct)

add_executable(vk264_test_shader_dc test_shader_dc.c)
target_link_libraries(vk264_test_shader_dc PRIVATE vk264_rewrite_ref vk264_rewrite_host)
target_compile_definitions(vk264_test_shader_dc PRIVATE
    VK264_SHADER_DIR="${CMAKE_BINARY_DIR}/shaders/")
add_dependencies(vk264_test_shader_dc vk264_rewrite_shaders)
add_test(NAME rewrite_shader_dc COMMAND vk264_test_shader_dc)

add_executable(vk264_test_shader_quant test_shader_quant.c)
target_link_libraries(vk264_test_shader_quant PRIVATE vk264_rewrite_ref vk264_rewrite_host)
target_compile_definitions(vk264_test_shader_quant PRIVATE
    VK264_SHADER_DIR="${CMAKE_BINARY_DIR}/shaders/")
add_dependencies(vk264_test_shader_quant vk264_rewrite_shaders)
add_test(NAME rewrite_shader_quant COMMAND vk264_test_shader_quant)

add_executable(vk264_test_shader_predict test_shader_predict.c)
target_link_libraries(vk264_test_shader_predict PRIVATE vk264_rewrite_ref vk264_rewrite_host)
target_compile_definitions(vk264_test_shader_predict PRIVATE
    VK264_SHADER_DIR="${CMAKE_BINARY_DIR}/shaders/")
add_dependencies(vk264_test_shader_predict vk264_rewrite_shaders)
add_test(NAME rewrite_shader_predict COMMAND vk264_test_shader_predict)
