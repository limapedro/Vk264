$ErrorActionPreference='Continue'
Set-Location 'E:\__PROJECTS__\Vk264-Subaru'
$exp='tests\02_mp4_vs_mp4_clean_benchmark'
$src="$exp\source.yuv"
$w=1920;$h=1080;$fps=60
$vk="$exp\vk264_g60_fixed.mp4"
$x="$exp\x264_g60.mp4"

# Extract decoded frames to raw YUV (120 frames)
foreach($f in @(@($vk,'vk_dec.yuv'),@($x,'x_dec.yuv'))) {
  & ffmpeg -y -v error -i $f[0] -frames:v 120 -pix_fmt yuv420p -f rawvideo "$exp\$($f[1])" 2>&1 | Out-Null
}
Write-Output 'extracted'
