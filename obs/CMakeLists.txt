set(WX_OBS_HEADERS "" CACHE PATH "Directory containing obs-module.h")
if(NOT EXISTS "${WX_OBS_HEADERS}/obs-module.h")
    message(FATAL_ERROR "Set WX_OBS_HEADERS to the OBS SDK libobs include directory")
endif()

set(obs_import "${CMAKE_CURRENT_BINARY_DIR}/obs.lib")
add_custom_command(OUTPUT "${obs_import}"
    COMMAND "${CMAKE_AR}" /NOLOGO /DEF:${CMAKE_CURRENT_SOURCE_DIR}/obs-imports.def
            /MACHINE:X64 /OUT:${obs_import}
    DEPENDS obs-imports.def VERBATIM)
add_custom_target(wx_obs_import DEPENDS "${obs_import}")

add_library(obs-wx264 MODULE obs-wx264.cpp ../src/gpu.cpp)
add_dependencies(obs-wx264 wx264_shaders wx_obs_import)
target_include_directories(obs-wx264 PRIVATE ../src "${WX_OBS_HEADERS}")
target_compile_features(obs-wx264 PRIVATE cxx_std_17)
target_compile_definitions(obs-wx264 PRIVATE WX_SHADER_DIR="${shader_dir}")
target_link_libraries(obs-wx264 PRIVATE "${obs_import}" Vulkan::Vulkan)

install(TARGETS obs-wx264 RUNTIME DESTINATION obs-wx264/bin/64bit
                         LIBRARY DESTINATION obs-wx264/bin/64bit)
install(FILES ${shaders} DESTINATION obs-wx264/data/shaders)
install(FILES locale/en-US.ini DESTINATION obs-wx264/data/locale)
install(FILES README.md ../src/OPENH264_LICENSE.txt DESTINATION obs-wx264)

add_executable(wx264_obs_smoke smoke.c)
add_dependencies(wx264_obs_smoke wx_obs_import)
target_include_directories(wx264_obs_smoke PRIVATE "${WX_OBS_HEADERS}")
target_link_libraries(wx264_obs_smoke PRIVATE "${obs_import}")
