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

# Vk264/H.264: a separately named encoder so it can coexist with the
# wx264 preview plugin in the same portable OBS installation.
add_library(obs-vk264 MODULE obs-vk264.cpp ../src/gpu.cpp)
add_dependencies(obs-vk264 wx264_shaders wx_obs_import)
target_include_directories(obs-vk264 PRIVATE ../src "${WX_OBS_HEADERS}")
target_compile_features(obs-vk264 PRIVATE cxx_std_17)
target_compile_definitions(obs-vk264 PRIVATE WX_SHADER_DIR="${shader_dir}")
target_link_libraries(obs-vk264 PRIVATE "${obs_import}" Vulkan::Vulkan)

# Portable OBS scans plugins/<name>/*.dll directly (system installs use
# bin/64bit).  Put the DLL at the plugin root so a portable build needs no
# restructuring; data/ is resolved relative to the module by libobs.
install(TARGETS obs-vk264 RUNTIME DESTINATION obs-vk264
                          LIBRARY DESTINATION obs-vk264)
install(FILES ${shaders} DESTINATION obs-vk264/data/shaders)
install(FILES obs-vk264-locale.ini DESTINATION obs-vk264/data/locale RENAME en-US.ini)
install(FILES README-vk264.md DESTINATION obs-vk264 RENAME README.md)
install(FILES ../src/OPENH264_LICENSE.txt DESTINATION obs-vk264)

add_executable(vk264_obs_smoke smoke-vk264.c)
add_dependencies(vk264_obs_smoke wx_obs_import)
target_include_directories(vk264_obs_smoke PRIVATE "${WX_OBS_HEADERS}")
target_link_libraries(vk264_obs_smoke PRIVATE "${obs_import}")
