$ErrorActionPreference='Continue'
Set-Location 'E:\__PROJECTS__\Vk264-Subaru'
$exp='tests\02_mp4_vs_mp4_clean_benchmark'
$src="$exp\source.yuv"
$frames=120; $w=1920; $h=1080; $fps=60; $kbps=6000

# encode raw .h264 (no muxer) then mux externally with ffmpeg to compare
& build\Release\vk264.exe $src "$exp\vk_raw.h264" $w $h $frames $fps --bitrate $kbps --gop 1 --backend vulkan --preset ultrafast 2>&1 | Out-Null
& ffmpeg -y -v error -fflags +genpts -r $fps -i "$exp\vk_raw.h264" -c copy "$exp\vk_external.mp4" 2>&1 | Out-Null

foreach ($f in 'vk264_g1.mp4','vk_external.mp4') {
  $p="$exp\$f"
  $decode=(& ffmpeg -v error -i $p -f null - 2>&1 | Select-Object -First 1)
  $psnr=(& ffmpeg -v info -f rawvideo -pixel_format yuv420p -video_size "${w}x${h}" -framerate $fps -i $src -i $p -lavfi psnr -f null - 2>&1 | Select-String 'PSNR y:').Line
  $nf=& ffprobe -v error -count_frames -select_streams v:0 -show_entries stream=nb_read_frames -of csv=p=0 $p 2>$null
  Write-Output "$f : frames=$nf size=$((Get-Item $p).Length) decode=$(if($decode){'ERR'}else{'CLEAN'})"
  Write-Output "  $psnr"
}
