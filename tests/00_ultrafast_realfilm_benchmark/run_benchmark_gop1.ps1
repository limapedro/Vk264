# Clean benchmark: vk264 ultrafast (gop=1, all-IDR) vs x264 ultrafast gop=1
$ErrorActionPreference='Continue'
Set-Location 'E:\__PROJECTS__\Vk264-Subaru'
$exp='tests\00_ultrafast_realfilm_benchmark'
$src="$exp\source_1920x1080_660f.yuv"
$frames=660; $w=1920; $h=1080; $fps=60; $qp=23

Write-Output "=== vk264 ultrafast gop=1 ==="
$sw=[System.Diagnostics.Stopwatch]::StartNew()
& build\Release\vk264.exe $src "$exp\vk264_gop1_full.h264" $w $h $frames $fps --qp $qp --gop 1 --backend vulkan --preset ultrafast 2>&1 |
  Tee-Object "$exp\vk264_gop1_stdout.txt" | Select-Object -Last 1
$sw.Stop(); $vkSec=$sw.Elapsed.TotalSeconds
$vkSize=(Get-Item "$exp\vk264_gop1_full.h264").Length
Write-Output ("vk264 gop1: {0:N2}s {1:N2}fps {2}bytes" -f $vkSec,($frames/$vkSec),$vkSize)

Write-Output "=== x264 ultrafast gop=1 ==="
$sw=[System.Diagnostics.Stopwatch]::StartNew()
& ffmpeg -y -v error -f rawvideo -pixel_format yuv420p -video_size "${w}x${h}" -framerate $fps -i $src `
  -frames:v $frames -an -c:v libx264 -preset ultrafast -qp $qp -g 1 -pix_fmt yuv420p -f h264 "$exp\x264_gop1_full.h264" 2>&1 | Out-Null
$sw.Stop(); $xSec=$sw.Elapsed.TotalSeconds
$xSize=(Get-Item "$exp\x264_gop1_full.h264").Length
Write-Output ("x264 gop1: {0:N2}s {1:N2}fps {2}bytes" -f $xSec,($frames/$xSec),$xSize)

Write-Output "=== decode check ==="
$vkErr=(& ffmpeg -v error -i "$exp\vk264_gop1_full.h264" -f null - 2>&1 | Measure-Object).Count
$xErr=(& ffmpeg -v error -i "$exp\x264_gop1_full.h264" -f null - 2>&1 | Measure-Object).Count
Write-Output "decode errors: vk264=$vkErr x264=$xErr"

Write-Output "=== PSNR/SSIM (vk264 gop1 vs source) ==="
$psnr = & ffmpeg -v info -f rawvideo -pixel_format yuv420p -video_size "${w}x${h}" -framerate $fps -i $src `
  -i "$exp\vk264_gop1_full.h264" -lavfi psnr -f null - 2>&1 | Select-String 'PSNR y:'
$ssim = & ffmpeg -v info -f rawvideo -pixel_format yuv420p -video_size "${w}x${h}" -framerate $fps -i $src `
  -i "$exp\vk264_gop1_full.h264" -lavfi ssim -f null - 2>&1 | Select-String 'SSIM Y:'
Write-Output "PSNR: $psnr"
Write-Output "SSIM: $ssim"

$summary = @{
  source='warzone2020_verdansk_1080p'; frames=$frames; width=$w; height=$h; fps=$fps; qp=$qp; gop=1
  vk264_seconds=$vkSec; vk264_fps=($frames/$vkSec); vk264_bytes=$vkSize; vk264_decode_errors=$vkErr
  x264_seconds=$xSec; x264_fps=($frames/$xSec); x264_bytes=$xSize; x264_decode_errors=$xErr
  speedup=($xSec/$vkSec); size_ratio=($vkSize/[Math]::Max(1,$xSize))
  psnr="$psnr"; ssim="$ssim"
  note='gop=60 P-slice stream has decode errors (see gop60_note.txt); gop=1 is clean'
  date=(Get-Date -Format o)
} | ConvertTo-Json
$summary | Out-File "$exp\summary_gop1.json" -Encoding UTF8
Get-Content "$exp\summary_gop1.json"
