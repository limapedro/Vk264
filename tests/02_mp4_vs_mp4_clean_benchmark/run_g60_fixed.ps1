$ErrorActionPreference='Continue'
Set-Location 'E:\__PROJECTS__\Vk264-Subaru'
$exp='tests\02_mp4_vs_mp4_clean_benchmark'
$src="$exp\source.yuv"
$frames=120; $w=1920; $h=1080; $fps=60; $kbps=6000

& build\Release\vk264.exe $src "$exp\vk264_g60_fixed.mp4" $w $h $frames $fps --bitrate $kbps --gop 60 --backend vulkan --preset ultrafast 2>&1 | Out-Null
$vkErr=(& ffmpeg -v error -i "$exp\vk264_g60_fixed.mp4" -f null - 2>&1 | Select-Object -First 1)
$psnr=(& ffmpeg -v info -f rawvideo -pixel_format yuv420p -video_size "${w}x${h}" -framerate $fps -i $src -i "$exp\vk264_g60_fixed.mp4" -lavfi psnr -f null - 2>&1 | Select-String 'PSNR y:').Line
$ssim=(& ffmpeg -v info -f rawvideo -pixel_format yuv420p -video_size "${w}x${h}" -framerate $fps -i $src -i "$exp\vk264_g60_fixed.mp4" -lavfi ssim -f null - 2>&1 | Select-String 'SSIM Y:').Line
Write-Output "decode: $(if($vkErr){$vkErr}else{'CLEAN'})"
Write-Output "size: $((Get-Item "$exp\vk264_g60_fixed.mp4").Length)"
Write-Output "PSNR: $psnr"
Write-Output "SSIM: $ssim"
