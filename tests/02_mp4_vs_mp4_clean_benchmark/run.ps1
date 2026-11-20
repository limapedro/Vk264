# MP4 vs MP4 clean comparison at 1080p60 6000kbps CBR
$ErrorActionPreference='Continue'
Set-Location 'E:\__PROJECTS__\Vk264-Subaru'
$exp='tests\02_mp4_vs_mp4_clean_benchmark'
$src="$exp\source.yuv"
$frames=120; $w=1920; $h=1080; $fps=60; $kbps=6000

foreach ($gop in 1,60) {
  Write-Output "===== GOP=$gop ====="
  # vk264 -> MP4 (native muxer)
  $sw=[System.Diagnostics.Stopwatch]::StartNew()
  & build\Release\vk264.exe $src "$exp\vk264_g${gop}.mp4" $w $h $frames $fps `
    --bitrate $kbps --gop $gop --backend vulkan --preset ultrafast 2>&1 | Select-Object -Last 1
  $sw.Stop(); $vkSec=$sw.Elapsed.TotalSeconds

  # x264 -> MP4 (ffmpeg native)
  $sw=[System.Diagnostics.Stopwatch]::StartNew()
  & ffmpeg -y -v error -f rawvideo -pixel_format yuv420p -video_size "${w}x${h}" -framerate $fps -i $src `
    -frames:v $frames -an -c:v libx264 -preset ultrafast -b:v ${kbps}k -minrate ${kbps}k `
    -maxrate ${kbps}k -bufsize ${kbps}k -x264-params "vbv-maxrate=$kbps:vbv-bufsize=$kbps" `
    -g $gop -pix_fmt yuv420p -movflags +faststart "$exp\x264_g${gop}.mp4" 2>&1 | Out-Null
  $sw.Stop(); $xSec=$sw.Elapsed.TotalSeconds

  # decode check
  $vkErr=(& ffmpeg -v error -i "$exp\vk264_g${gop}.mp4" -f null - 2>&1 | Select-Object -First 1)
  $xErr=(& ffmpeg -v error -i "$exp\x264_g${gop}.mp4" -f null - 2>&1 | Select-Object -First 1)

  # PSNR / SSIM of vk264 vs source
  $psnr=(& ffmpeg -v info -f rawvideo -pixel_format yuv420p -video_size "${w}x${h}" -framerate $fps -i $src `
    -i "$exp\vk264_g${gop}.mp4" -lavfi psnr -f null - 2>&1 | Select-String 'PSNR y:').Line
  $ssim=(& ffmpeg -v info -f rawvideo -pixel_format yuv420p -video_size "${w}x${h}" -framerate $fps -i $src `
    -i "$exp\vk264_g${gop}.mp4" -lavfi ssim -f null - 2>&1 | Select-String 'SSIM Y:').Line

  Write-Output ("vk: {0:N2}s {1:N2}fps size={2} decode={3}" -f $vkSec,($frames/$vkSec),(Get-Item "$exp\vk264_g${gop}.mp4").Length,$(if($vkErr){$vkErr}else{'CLEAN'}))
  Write-Output ("x : {0:N2}s {1:N2}fps size={2} decode={3}" -f $xSec,($frames/$xSec),(Get-Item "$exp\x264_g${gop}.mp4").Length,$(if($xErr){$xErr}else{'CLEAN'}))
  Write-Output "PSNR: $psnr"
  Write-Output "SSIM: $ssim"
}
