$ErrorActionPreference='Continue'
Set-Location 'E:\__PROJECTS__\Vk264-Subaru'
$exp='tests\00_ultrafast_realfilm_benchmark'
$src="$exp\source_1920x1080_660f.yuv"
foreach ($gop in 1,60) {
  $out="$exp\vk264_gop${gop}_30f.h264"
  & build\Release\vk264.exe $src $out 1920 1080 30 60 --qp 23 --gop $gop --backend vulkan --preset ultrafast 2>&1 | Out-Null
  $err = (& ffmpeg -v error -i $out -f null - 2>&1 | Select-Object -First 1)
  if ($LASTEXITCODE -eq 0 -and -not $err) { $status='CLEAN' } else { $status="ERRORS: $err" }
  Write-Output "gop=$gop frames=30 size=$((Get-Item $out).Length) decode=$status"
}
