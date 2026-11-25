# Extended parity map (longer clips, multiple regions)

Cases: 240 (2 clips x 3 offsets x 2 frame counts x 10 presets x 2 entropies).
Byte-exact: 0/240 = 0.0%
NAL-count mismatches: 120

## By preset

| preset | byte-exact | NAL-count | other |
|---|---:|---:|---:|
| ultrafast | 0/24 | 0/24 | 24/24 |
| superfast | 0/24 | 14/24 | 10/24 |
| veryfast | 0/24 | 14/24 | 10/24 |
| faster | 0/24 | 14/24 | 10/24 |
| fast | 0/24 | 14/24 | 10/24 |
| medium | 0/24 | 14/24 | 10/24 |
| slow | 0/24 | 14/24 | 10/24 |
| slower | 0/24 | 14/24 | 10/24 |
| veryslow | 0/24 | 11/24 | 13/24 |
| placebo | 0/24 | 11/24 | 13/24 |

## Verdict distribution

- ENCODE FAIL: 54
- NAL35 byte1: 24
- NAL COUNT 66/64: 20
- NAL COUNT 130/128: 16
- NAL3 byte5: 16
- NAL COUNT 68/64: 12
- NAL COUNT 72/70: 10
- NAL COUNT 70/68: 10
- NAL COUNT 68/66: 10
- NAL COUNT 78/70: 10
- NAL3 byte6: 8
- NAL COUNT 136/132: 6
- NAL COUNT 148/144: 5
- NAL COUNT 142/136: 5
- NAL COUNT 158/142: 5
- NAL3 byte40: 5
- NAL COUNT 74/70: 4
- NAL3 byte4: 3
- NAL3 byte8: 2
- NAL COUNT 78/66: 2
