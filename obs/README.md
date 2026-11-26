# Wx264 H.264 for OBS

This Windows x64 preview uses the same Vulkan shaders and presets as the
Wx264 command-line encoder. It was built against the OBS 33 libobs SDK.

1. Close OBS Studio. Extract the package and copy the entire obs-wx264
   folder into C:/ProgramData/obs-studio/plugins. The folder contains
   bin/64bit/obs-wx264.dll and data/shaders.
2. Reopen OBS. In Settings > Video set an even output resolution up to
   1920x1080 and an integer frame rate up to 60 fps.
3. In Settings > Output, choose Advanced mode, then Recording, then
   Wx264 H.264 (Vulkan) as the video encoder.
4. Start with the slow preset and the default 6000 kbps average bitrate.
   Check encoding lag in View > Stats while recording.

The plugin offers fast, medium, slow and veryslow, an average bitrate target
or constant QP, and a keyframe interval. Stop and restart the output to apply
changes. OBS converts the source to 8-bit I420 before passing it to Wx264.
The GPU needs a Vulkan 1.1 driver. Average bitrate can exceed the requested
rate over short windows. The slow and veryslow presets can signal H.264
levels 5.0 and 5.1 at 1080p. Check streaming-service restrictions before
using this preview live.

## Build from the Wx264 repository

    cmake -S . -B build-obs -A x64 -DWX_BUILD_OBS=ON -DWX_OBS_HEADERS=PATH_TO_LIBOBS_HEADERS
    cmake --build build-obs --config Release
    cmake --install build-obs --config Release --prefix dist

Set WX_OBS_HEADERS to an OBS 33 SDK directory containing obs-module.h and
obs-config.h. The optional build uses MSVC and the Vulkan SDK. The normal
Wx264 CLI build can omit WX_BUILD_OBS.
