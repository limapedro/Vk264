# Per-preset breakdown (x264 vs oracle)

Definition of byte-exact here: the concatenated H.264 NAL payloads (SPS/PPS/slices) are byte-identical. Raw .h264 and MP4 files may differ in padding/container metadata.

| clip | res | frames | preset | entropy | x264 NAL bytes | oracle NAL bytes | ratio | x264 NALs | oracle NALs | NAL-exact pairs |
|---|---|---|---|---|---|---|---|---|---|---|
| small64 | 64x64 | 2 | fast | cabac | 2724 | 1078 | 0.396 | 12 | 12 | 0/3 |
| small64 | 64x64 | 8 | fast | cabac | 5313 | 3819 | 0.719 | 32 | 30 | 0/3 |
| fulllen600 | 64x64 | 600 | fast | cabac | 137325 | 150063 | 1.093 | 1458 | 1204 | 0/2 |
| small64 | 64x64 | 2 | fast | cavlc | 2724 | 1375 | 0.505 | 12 | 12 | 0/3 |
| small64 | 64x64 | 8 | fast | cavlc | 5313 | 5392 | 1.015 | 32 | 30 | 0/3 |
| fulllen600 | 64x64 | 600 | fast | cavlc | 137325 | 250086 | 1.821 | 1458 | 1204 | 0/2 |
| small64 | 64x64 | 2 | faster | cabac | 2757 | 1078 | 0.391 | 12 | 12 | 0/3 |
| small64 | 64x64 | 8 | faster | cabac | 5301 | 3819 | 0.72 | 32 | 30 | 0/3 |
| fulllen600 | 64x64 | 600 | faster | cabac | 139884 | 150063 | 1.073 | 1458 | 1204 | 0/2 |
| small64 | 64x64 | 2 | faster | cavlc | 2757 | 1375 | 0.499 | 12 | 12 | 0/3 |
| small64 | 64x64 | 8 | faster | cavlc | 5301 | 5392 | 1.017 | 32 | 30 | 0/3 |
| fulllen600 | 64x64 | 600 | faster | cavlc | 139884 | 250086 | 1.788 | 1458 | 1204 | 0/2 |
| small64 | 64x64 | 2 | medium | cabac | 2736 | 1078 | 0.394 | 12 | 12 | 0/3 |
| small64 | 64x64 | 8 | medium | cabac | 5347 | 3819 | 0.714 | 32 | 30 | 0/3 |
| fulllen600 | 64x64 | 600 | medium | cabac | 139102 | 150063 | 1.079 | 1458 | 1204 | 0/2 |
| small64 | 64x64 | 2 | medium | cavlc | 2736 | 1376 | 0.503 | 12 | 12 | 0/3 |
| small64 | 64x64 | 8 | medium | cavlc | 5347 | 5397 | 1.009 | 32 | 30 | 0/3 |
| fulllen600 | 64x64 | 600 | medium | cavlc | 139102 | 250473 | 1.801 | 1458 | 1204 | 0/2 |
| small64 | 64x64 | 2 | placebo | cabac | 2740 | 1087 | 0.397 | 12 | 12 | 0/3 |
| small64 | 64x64 | 8 | placebo | cabac | 4908 | 3828 | 0.78 | 30 | 30 | 0/3 |
| fulllen600 | 64x64 | 600 | placebo | cabac | 125169 | 150107 | 1.199 | 1284 | 1204 | 0/2 |
| small64 | 64x64 | 2 | placebo | cavlc | 2740 | 1396 | 0.509 | 12 | 12 | 0/3 |
| small64 | 64x64 | 8 | placebo | cavlc | 4908 | 5494 | 1.119 | 30 | 30 | 0/3 |
| fulllen600 | 64x64 | 600 | placebo | cavlc | 125169 | 255544 | 2.042 | 1284 | 1204 | 0/2 |
| small64 | 64x64 | 2 | slow | cabac | 2741 | 1078 | 0.393 | 12 | 12 | 0/3 |
| small64 | 64x64 | 8 | slow | cabac | 5381 | 3819 | 0.71 | 32 | 30 | 0/3 |
| fulllen600 | 64x64 | 600 | slow | cabac | 139274 | 150063 | 1.077 | 1458 | 1204 | 0/2 |
| small64 | 64x64 | 2 | slow | cavlc | 2741 | 1378 | 0.503 | 12 | 12 | 0/3 |
| small64 | 64x64 | 8 | slow | cavlc | 5381 | 5406 | 1.005 | 32 | 30 | 0/3 |
| fulllen600 | 64x64 | 600 | slow | cavlc | 139274 | 251009 | 1.802 | 1458 | 1204 | 0/2 |
| small64 | 64x64 | 2 | slower | cabac | 2734 | 1081 | 0.395 | 12 | 12 | 0/3 |
| small64 | 64x64 | 8 | slower | cabac | 4968 | 3822 | 0.769 | 30 | 30 | 0/3 |
| fulllen600 | 64x64 | 600 | slower | cabac | 127973 | 150065 | 1.173 | 1304 | 1204 | 0/2 |
| small64 | 64x64 | 2 | slower | cavlc | 2734 | 1384 | 0.506 | 12 | 12 | 0/3 |
| small64 | 64x64 | 8 | slower | cavlc | 4968 | 5426 | 1.092 | 30 | 30 | 0/3 |
| fulllen600 | 64x64 | 600 | slower | cavlc | 127973 | 251983 | 1.969 | 1304 | 1204 | 0/2 |
| small64 | 64x64 | 2 | superfast | cabac | 2820 | 1075 | 0.381 | 12 | 12 | 0/3 |
| small64 | 64x64 | 8 | superfast | cabac | 5390 | 3816 | 0.708 | 34 | 30 | 0/3 |
| fulllen600 | 64x64 | 600 | superfast | cabac | 140375 | 150061 | 1.069 | 1426 | 1204 | 0/2 |
| small64 | 64x64 | 2 | superfast | cavlc | 2820 | 1372 | 0.487 | 12 | 12 | 0/3 |
| small64 | 64x64 | 8 | superfast | cavlc | 5390 | 5382 | 0.999 | 34 | 30 | 0/3 |
| fulllen600 | 64x64 | 600 | superfast | cavlc | 140375 | 249847 | 1.78 | 1426 | 1204 | 0/2 |
| small64 | 64x64 | 2 | ultrafast | cabac | 2689 | 1060 | 0.394 | 12 | 12 | 3/3 |
| small64 | 64x64 | 8 | ultrafast | cabac | 5412 | 3783 | 0.699 | 30 | 30 | 3/3 |
| fulllen600 | 64x64 | 600 | ultrafast | cabac | 151143 | 148875 | 0.985 | 1280 | 1204 | 0/2 |
| small64 | 64x64 | 2 | ultrafast | cavlc | 2836 | 1207 | 0.426 | 12 | 12 | 3/3 |
| small64 | 64x64 | 8 | ultrafast | cavlc | 5863 | 4234 | 0.722 | 30 | 30 | 3/3 |
| fulllen600 | 64x64 | 600 | ultrafast | cavlc | 164558 | 162289 | 0.986 | 1280 | 1204 | 0/2 |
| small64 | 64x64 | 2 | veryfast | cabac | 2755 | 1075 | 0.39 | 12 | 12 | 0/3 |
| small64 | 64x64 | 8 | veryfast | cabac | 5277 | 3816 | 0.723 | 32 | 30 | 0/3 |
| fulllen600 | 64x64 | 600 | veryfast | cabac | 137823 | 150061 | 1.089 | 1458 | 1204 | 0/2 |
| small64 | 64x64 | 2 | veryfast | cavlc | 2755 | 1372 | 0.498 | 12 | 12 | 0/3 |
| small64 | 64x64 | 8 | veryfast | cavlc | 5277 | 5382 | 1.02 | 32 | 30 | 0/3 |
| fulllen600 | 64x64 | 600 | veryfast | cavlc | 137823 | 249847 | 1.813 | 1458 | 1204 | 0/2 |
| small64 | 64x64 | 2 | veryslow | cabac | 2740 | 1084 | 0.396 | 12 | 12 | 0/3 |
| small64 | 64x64 | 8 | veryslow | cabac | 4963 | 3825 | 0.771 | 30 | 30 | 0/3 |
| fulllen600 | 64x64 | 600 | veryslow | cabac | 125325 | 150087 | 1.198 | 1284 | 1204 | 0/2 |
| small64 | 64x64 | 2 | veryslow | cavlc | 2740 | 1396 | 0.509 | 12 | 12 | 0/3 |
| small64 | 64x64 | 8 | veryslow | cavlc | 4963 | 5494 | 1.107 | 30 | 30 | 0/3 |
| fulllen600 | 64x64 | 600 | veryslow | cavlc | 125325 | 255539 | 2.039 | 1284 | 1204 | 0/2 |
