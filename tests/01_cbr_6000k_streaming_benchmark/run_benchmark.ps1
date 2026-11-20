# Experiment 01: 1080p60 CBR 6000 kbps streaming benchmark (vk264 vs x264)
$ErrorActionPreference='Continue'
Set-Location 'E:\__PROJECTS__\Vk264-Subaru'
$exp='tests\01_cbr_6000k_streaming_benchmark'
$src="$exp\source_1920x1080_660f.yuv"
$frames=660; $w=1920; $h=1080; $fps=60; $kbps=6000; $gop=60

Write-Output "=== vk264 ultrafast CBR ${kbps}k gop=$gop ==="
$sw=[System.Diagnostics.Stopwatch]::StartNew()
& build\Release\vk264.exe $src "$exp\vk264_cbr6000.h264" $w $h $frames $fps `
  --bitrate $kbps --gop $gop --backend vulkan --preset ultrafast 2>&1 |
  Tee-Object "$exp\vk264_stdout.txt" | Select-Object -Last 1
$sw.Stop(); $vkSec=$sw.Elapsed.TotalSeconds
$vkSize=(Get-Item "$exp\vk264_cbr6000.h264").Length
$vkKbps=[Math]::Round($vkSize*8/$frames/$fps/1000,1)
Write-Output ("vk264: {0:N2}s {1:N2}fps {2}bytes ({3} kbps actual)" -f $vkSec,($frames/$vkSec),$vkSize,$vkKbps)

Write-Output "=== x264 ultrafast CBR ${kbps}k gop=$gop ==="
$sw=[System.Diagnostics.Stopwatch]::StartNew()
& ffmpeg -y -v error -f rawvideo -pixel_format yuv420p -video_size "${w}x${h}" -framerate $fps -i $src `
  -frames:v $frames -an -c:v libx264 -preset ultrafast -b:v ${kbps}k -minrate ${kbps}k `
  -maxrate ${kbps}k -bufsize ${kbps}k -x264-params "vbv-maxrate=$kbps:vbv-bufsize=$kbps" `
  -g $gop -pix_fmt yuv420p -f h264 "$exp\x264_cbr6000.h264" 2>&1 |
  Tee-Object "$exp\x264_stderr.txt" | Select-Object -Last 2
$sw.Stop(); $xSec=$sw.Elapsed.TotalSeconds
$xSize=(Get-Item "$exp\x264_cbr6000.h264").Length
$xKbps=[Math]::Round($xSize*8/$frames/$fps/1000,1)
Write-Output ("x264: {0:N2}s {1:N2}fps {2}bytes ({3} kbps actual)" -f $xSec,($frames/$xSec),$xSize,$xKbps)

Write-Output "=== decode check ==="
$vkErr=(& ffmpeg -v error -i "$exp\vk264_cbr6000.h264" -f null - 2>&1 | Select-Object -First 1)
$xErr=(& ffmpeg -v error -i "$exp\x264_cbr6000.h264" -f null - 2>&1 | Select-Object -First 1)
Write-Output "vk264 decode: $(if($vkErr){$vkErr}else{'CLEAN'})"
Write-Output "x264 decode: $(if($xErr){$xErr}else{'CLEAN'})"

Write-Output "=== quality metrics ==="
$psnr = & ffmpeg -v info -f rawvideo -pixel_format yuv420p -video_size "${w}x${h}" -framerate $fps -i $src `
  -i "$exp\vk264_cbr6000.h264" -lavfi psnr -f null - 2>&1 | Select-String 'PSNR y:'
$ssim = & ffmpeg -v info -f rawvideo -pixel_format yuv420p -video_size "${w}x${h}" -framerate $fps -i $src `
  -i "$exp\vk264_cbr6000.h264" -lavfi ssim -f null - 2>&1 | Select-String 'SSIM Y:'
Write-Output "PSNR: $psnr"
Write-Output "SSIM: $ssim"

Write-Output "=== muxing correctly-timed MP4s ==="
foreach($f in 'vk264_cbr6000','x264_cbr6000') {
  & ffmpeg -y -v error -fflags +genpts -r $fps -i "$exp\$f.h264" -c copy "$exp\$f.mp4" 2>&1 | Out-Null
  $nf=& ffprobe -v error -count_frames -select_streams v:0 -show_entries stream=nb_read_frames -of csv=p=0 "$exp\$f.mp4" 2>$null
  Write-Output "$f.mp4 : $nf frames"
}

$summary=@{
  source='warzone2020_verdansk_1080p'; frames=$frames; width=$w; height=$h; fps=$fps
  target_kbps=$kbps; gop=$gop; preset='ultrafast'
  vk264_seconds=$vkSec; vk264_fps=($frames/$vkSec); vk264_bytes=$vkSize; vk264_actual_kbps=$vkKbps
  x264_seconds=$xSec; x264_fps=($frames/$xSec); x264_bytes=$xSize; x264_actual_kbps=$xKbps
  speedup=($xSec/$vkSec); size_ratio=($vkSize/[Math]::Max(1,$xSize))
  vk264_decode=$(if($vkErr){'ERRORS'}else{'CLEAN'}); x264_decode=$(if($xErr){'ERRORS'}else{'CLEAN'})
  psnr="$psnr"; ssim="$ssim"; date=(Get-Date -Format o)
} | ConvertTo-Json
$summary | Out-File "$exp\summary.json" -Encoding UTF8
Get-Content "$exp\summary.json"
