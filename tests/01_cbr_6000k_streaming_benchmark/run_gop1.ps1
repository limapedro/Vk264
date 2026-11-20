# CBR 6000k clean comparison: GOP=1 (all-IDR) avoids the P-slice corruption
$ErrorActionPreference='Continue'
Set-Location 'E:\__PROJECTS__\Vk264-Subaru'
$exp='tests\01_cbr_6000k_streaming_benchmark'
$src="$exp\source_1920x1080_660f.yuv"
$frames=660; $w=1920; $h=1080; $fps=60; $kbps=6000

# actual kbps = bytes*8 / (frames/fps) / 1000 = bytes*8*fps/frames/1000
function Get-Kbps($bytes){ [Math]::Round($bytes*8*$fps/$frames/1000,1) }

Write-Output "=== vk264 CBR ${kbps}k gop=1 ==="
$sw=[System.Diagnostics.Stopwatch]::StartNew()
& build\Release\vk264.exe $src "$exp\vk264_cbr6000_gop1.h264" $w $h $frames $fps `
  --bitrate $kbps --gop 1 --backend vulkan --preset ultrafast 2>&1 |
  Tee-Object "$exp\vk264_gop1_stdout.txt" | Select-Object -Last 1
$sw.Stop(); $vkSec=$sw.Elapsed.TotalSeconds
$vkSize=(Get-Item "$exp\vk264_cbr6000_gop1.h264").Length
Write-Output ("vk264: {0:N2}s {1:N2}fps {2}bytes ({3} kbps)" -f $vkSec,($frames/$vkSec),$vkSize,(Get-Kbps $vkSize))

Write-Output "=== x264 CBR ${kbps}k gop=1 ==="
$sw=[System.Diagnostics.Stopwatch]::StartNew()
& ffmpeg -y -v error -f rawvideo -pixel_format yuv420p -video_size "${w}x${h}" -framerate $fps -i $src `
  -frames:v $frames -an -c:v libx264 -preset ultrafast -b:v ${kbps}k -minrate ${kbps}k `
  -maxrate ${kbps}k -bufsize ${kbps}k -x264-params "vbv-maxrate=$kbps:vbv-bufsize=$kbps" `
  -g 1 -pix_fmt yuv420p -f h264 "$exp\x264_cbr6000_gop1.h264" 2>&1 | Out-Null
$sw.Stop(); $xSec=$sw.Elapsed.TotalSeconds
$xSize=(Get-Item "$exp\x264_cbr6000_gop1.h264").Length
Write-Output ("x264: {0:N2}s {1:N2}fps {2}bytes ({3} kbps)" -f $xSec,($frames/$xSec),$xSize,(Get-Kbps $xSize))

Write-Output "=== decode check ==="
$vkErr=(& ffmpeg -v error -i "$exp\vk264_cbr6000_gop1.h264" -f null - 2>&1 | Select-Object -First 1)
$xErr=(& ffmpeg -v error -i "$exp\x264_cbr6000_gop1.h264" -f null - 2>&1 | Select-Object -First 1)
Write-Output "vk264: $(if($vkErr){$vkErr}else{'CLEAN'})"
Write-Output "x264 : $(if($xErr){$xErr}else{'CLEAN'})"

Write-Output "=== quality ==="
$psnr = & ffmpeg -v info -f rawvideo -pixel_format yuv420p -video_size "${w}x${h}" -framerate $fps -i $src `
  -i "$exp\vk264_cbr6000_gop1.h264" -lavfi psnr -f null - 2>&1 | Select-String 'PSNR y:'
$ssim = & ffmpeg -v info -f rawvideo -pixel_format yuv420p -video_size "${w}x${h}" -framerate $fps -i $src `
  -i "$exp\vk264_cbr6000_gop1.h264" -lavfi ssim -f null - 2>&1 | Select-String 'SSIM Y:'
Write-Output "PSNR: $psnr"
Write-Output "SSIM: $ssim"

foreach($f in 'vk264_cbr6000_gop1','x264_cbr6000_gop1') {
  & ffmpeg -y -v error -fflags +genpts -r $fps -i "$exp\$f.h264" -c copy "$exp\$f.mp4" 2>&1 | Out-Null
}

$summary=@{
  frames=$frames; fps=$fps; target_kbps=$kbps; gop=1; preset='ultrafast'
  vk264_seconds=$vkSec; vk264_fps=($frames/$vkSec); vk264_bytes=$vkSize; vk264_kbps=(Get-Kbps $vkSize)
  x264_seconds=$xSec; x264_fps=($frames/$xSec); x264_bytes=$xSize; x264_kbps=(Get-Kbps $xSize)
  speedup=($xSec/$vkSec); size_ratio=($vkSize/[Math]::Max(1,$xSize))
  vk264_decode=$(if($vkErr){'ERRORS'}else{'CLEAN'}); x264_decode=$(if($xErr){'ERRORS'}else{'CLEAN'})
  psnr="$psnr"; ssim="$ssim"; date=(Get-Date -Format o)
} | ConvertTo-Json
$summary | Out-File "$exp\summary_gop1.json" -Encoding UTF8
Get-Content "$exp\summary_gop1.json"
