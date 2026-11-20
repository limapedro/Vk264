# Benchmark: vk264 ultrafast vs x264 ultrafast on real 1080p footage
# Source: warzone2020_verdansk (660 frames, 1920x1080, ~59.94 fps)
$ErrorActionPreference = 'Continue'
Set-Location 'E:\__PROJECTS__\Vk264-Subaru'
$exp = 'tests\00_ultrafast_realfilm_benchmark'
$src = "$exp\source_1920x1080_660f.yuv"
$frames = 660
$w = 1920; $h = 1080; $fps = 60
$qp = 23
$results = @()

function Get-StreamSize($path) { if (Test-Path $path) { (Get-Item $path).Length } else { 0 } }

Write-Output "=== vk264 ultrafast (vulkan backend) ==="
$sw = [System.Diagnostics.Stopwatch]::StartNew()
& build\Release\vk264.exe $src "$exp\vk264_ultrafast.h264" $w $h $frames $fps --qp $qp --gop 60 --backend vulkan --preset ultrafast 2>&1 |
    Tee-Object "$exp\vk264_stdout.txt" | Select-Object -Last 2
$sw.Stop()
$vkSec = $sw.Elapsed.TotalSeconds
$vkSize = Get-StreamSize "$exp\vk264_ultrafast.h264"
Write-Output ("vk264: {0:N2}s, {1:N2} fps, {2} bytes" -f $vkSec, ($frames/$vkSec), $vkSize)

Write-Output "=== x264 ultrafast (ffmpeg libx264) ==="
$sw = [System.Diagnostics.Stopwatch]::StartNew()
& ffmpeg -y -v error -f rawvideo -pixel_format yuv420p -video_size "${w}x${h}" -framerate $fps -i $src -frames:v $frames -an `
    -c:v libx264 -preset ultrafast -qp $qp -g 60 -pix_fmt yuv420p -f h264 "$exp\x264_ultrafast.h264" 2>&1 |
    Tee-Object "$exp\x264_stderr.txt" | Select-Object -Last 3
$sw.Stop()
$x264Sec = $sw.Elapsed.TotalSeconds
$x264Size = Get-StreamSize "$exp\x264_ultrafast.h264"
Write-Output ("x264: {0:N2}s, {1:N2} fps, {2} bytes" -f $x264Sec, ($frames/$x264Sec), $x264Size)

Write-Output "=== quality metrics (ffmpeg PSNR/SSIM vs source) ==="
& ffmpeg -v error -f rawvideo -pixel_format yuv420p -video_size "${w}x${h}" -framerate $fps -i $src `
    -i "$exp\vk264_ultrafast.h264" -lavfi "psnr=stats_file=$exp\psnr_vk.txt" -f null - 2>&1 |
    Tee-Object "$exp\psnr_vk_stderr.txt" | Select-Object -Last 3
& ffmpeg -v error -f rawvideo -pixel_format yuv420p -video_size "${w}x${h}" -framerate $fps -i $src `
    -i "$exp\vk264_ultrafast.h264" -lavfi "ssim=stats_file=$exp\ssim_vk.txt" -f null - 2>&1 |
    Tee-Object "$exp\ssim_vk_stderr.txt" | Select-Object -Last 3

# Summary JSON
$summary = @{
  source = 'warzone2020_verdansk_4k_src_1080p_nvenc_p7_24mbps.mp4'
  frames = $frames; width = $w; height = $h; fps = $fps; qp = $qp; gop = 60
  vk264_seconds = $vkSec; vk264_fps = ($frames/$vkSec); vk264_bytes = $vkSize
  x264_seconds = $x264Sec; x264_fps = ($frames/$x264Sec); x264_bytes = $x264Size
  speedup = ($x264Sec/$vkSec)
  size_ratio = ($vkSize / [Math]::Max(1,$x264Size))
  date = (Get-Date -Format o)
} | ConvertTo-Json
$summary | Out-File "$exp\summary.json" -Encoding UTF8
Write-Output "=== summary.json ==="
Get-Content "$exp\summary.json"
