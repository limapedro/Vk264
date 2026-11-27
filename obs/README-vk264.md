# Vk264/H.264 for OBS (portable)

Vk264/H.264 is a Windows x64 OBS encoder plugin built from the same Vulkan
shaders, presets and rate control as the Vk264 command-line encoder. It is
built against the OBS 33 libobs SDK and installs as a self-contained folder,
so it can be dropped into a portable OBS installation without touching the
system-wide plugin directory.

## Install into portable OBS

1. Close portable OBS.
2. Copy the built `obs-vk264` folder into
   `<portable-obs>/plugins/obs-vk264`. It must contain:

       obs-vk264.dll
       data/shaders/*.spv
       data/locale/en-US.ini

3. Start portable OBS with its existing launcher (for example
   `Launch-Wx264-OBS.bat`, which starts `obs64.exe --portable --multi
   --disable-updater`). The new encoder appears independently of any
   installed `wx264` plugin.
4. Settings > Video: use an even resolution up to 1920x1080 and an integer
   frame rate up to 60 fps.
5. Settings > Output > Advanced > Recording: choose **Vk264/H.264** as the
   video encoder. Start with `slow`, ABR 6000 kbps and a 5 s keyframe
   interval. Stop and restart the output after changing settings.
6. Watch View > Stats for encoding lag while recording.

OBS converts the source to 8-bit I420 before passing it to Vk264. A Vulkan
1.1 capable GPU/driver is required. The keyframe interval is in seconds and
maps to `gop = seconds * fps`. Average bitrate may exceed the target over
short windows. `slow` and `veryslow` may signal H.264 levels 5.0/5.1 at
1080p; check service limits before using this preview live.

## Build

    cmake -S . -B build-obs -A x64 -DWX_BUILD_OBS=ON -DWX_OBS_HEADERS=PATH_TO_LIBOBS_INCLUDE
    cmake --build build-obs --config Release
    cmake --install build-obs --config Release --prefix dist

`PATH_TO_LIBOBS_INCLUDE` is a directory containing `obs-module.h` (for
example `.../obs-studio-33.0.0/libobs`). The install tree is
`dist/obs-vk264`; copy that whole folder into portable OBS. The normal Vk264
CLI build does not need `WX_BUILD_OBS`.

## Verify without the OBS GUI

The `vk264_obs_smoke` test loads the built DLL through real libobs, encodes
240 frames of a 1920x1080 I420 source at 60 fps, checks packet/keyframe/
timestamp invariants and writes an Annex-B H.264 stream:

    vk264_obs_smoke.exe obs-vk264.dll <data-dir> input.yuv out.h264 slow i420
