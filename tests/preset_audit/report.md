# Fresh preset audit

12/120 byte-exact

| exp | clip | frames | preset | entropy | verdict | x264 PSNR | oracle PSNR | oracle green blocks |
|---|---|---|---|---|---|---|---|---|
| 001 | warzone2020 | 2 | ultrafast | cavlc | BYTE-EXACT | 45.61 | 45.61 | 0 |
| 002 | warzone2020 | 2 | ultrafast | cabac | BYTE-EXACT | 45.61 | 45.61 | 0 |
| 003 | warzone2020 | 2 | superfast | cavlc | NAL2 byte4 | -1 | -1 | -1 |
| 004 | warzone2020 | 2 | superfast | cabac | NAL2 byte49 | -1 | -1 | -1 |
| 005 | warzone2020 | 2 | veryfast | cavlc | NAL2 byte4 | -1 | -1 | -1 |
| 006 | warzone2020 | 2 | veryfast | cabac | NAL2 byte16 | -1 | -1 | -1 |
| 007 | warzone2020 | 2 | faster | cavlc | NAL2 byte4 | -1 | -1 | -1 |
| 008 | warzone2020 | 2 | faster | cabac | NAL2 byte16 | -1 | -1 | -1 |
| 009 | warzone2020 | 2 | fast | cavlc | NAL2 byte10 | -1 | -1 | -1 |
| 010 | warzone2020 | 2 | fast | cabac | NAL2 byte5 | -1 | -1 | -1 |
| 011 | warzone2020 | 2 | medium | cavlc | NAL2 byte10 | -1 | -1 | -1 |
| 012 | warzone2020 | 2 | medium | cabac | NAL2 byte5 | -1 | -1 | -1 |
| 013 | warzone2020 | 2 | slow | cavlc | NAL2 byte10 | -1 | -1 | -1 |
| 014 | warzone2020 | 2 | slow | cabac | NAL2 byte5 | -1 | -1 | -1 |
| 015 | warzone2020 | 2 | slower | cavlc | NAL2 byte10 | -1 | -1 | -1 |
| 016 | warzone2020 | 2 | slower | cabac | NAL2 byte5 | -1 | -1 | -1 |
| 017 | warzone2020 | 2 | veryslow | cavlc | NAL2 byte10 | -1 | - | - |
| 018 | warzone2020 | 2 | veryslow | cabac | NAL2 byte6 | -1 | - | - |
| 019 | warzone2020 | 2 | placebo | cavlc | NAL2 byte10 | -1 | - | - |
| 020 | warzone2020 | 2 | placebo | cabac | NAL2 byte6 | -1 | -1 | -1 |
| 021 | warzone2020 | 8 | ultrafast | cavlc | BYTE-EXACT | 40.15 | 40.15 | 0 |
| 022 | warzone2020 | 8 | ultrafast | cabac | BYTE-EXACT | 40.15 | 40.15 | 0 |
| 023 | warzone2020 | 8 | superfast | cavlc | NAL2 byte4 | 24.49 | 21.86 | 0 |
| 024 | warzone2020 | 8 | superfast | cabac | NAL2 byte49 | 24.49 | 9.52 | 192 |
| 025 | warzone2020 | 8 | veryfast | cavlc | NAL2 byte4 | 24.51 | 21.86 | 0 |
| 026 | warzone2020 | 8 | veryfast | cabac | NAL2 byte16 | 24.52 | 9.52 | 192 |
| 027 | warzone2020 | 8 | faster | cavlc | NAL2 byte4 | 24.5 | 22.16 | 0 |
| 028 | warzone2020 | 8 | faster | cabac | NAL2 byte16 | 24.48 | 9.43 | 320 |
| 029 | warzone2020 | 8 | fast | cavlc | NAL2 byte10 | 24.46 | 22.17 | 0 |
| 030 | warzone2020 | 8 | fast | cabac | NAL2 byte5 | 24.42 | 9.43 | 320 |
| 031 | warzone2020 | 8 | medium | cavlc | NAL2 byte10 | 24.46 | 22.2 | 0 |
| 032 | warzone2020 | 8 | medium | cabac | NAL2 byte5 | 24.43 | 9.41 | 384 |
| 033 | warzone2020 | 8 | slow | cavlc | NAL2 byte10 | 24.4 | 22.2 | 0 |
| 034 | warzone2020 | 8 | slow | cabac | NAL2 byte5 | 24.43 | 9.47 | 315 |
| 035 | warzone2020 | 8 | slower | cavlc | NAL2 byte10 | 24.69 | 22.2 | 0 |
| 036 | warzone2020 | 8 | slower | cabac | NAL2 byte5 | 24.7 | 9.48 | 374 |
| 037 | warzone2020 | 8 | veryslow | cavlc | NAL2 byte10 | 28.74 | - | - |
| 038 | warzone2020 | 8 | veryslow | cabac | NAL2 byte6 | 28.83 | - | - |
| 039 | warzone2020 | 8 | placebo | cavlc | NAL2 byte10 | 24.24 | - | - |
| 040 | warzone2020 | 8 | placebo | cabac | NAL2 byte6 | 24.24 | - | - |
| 041 | warzone2020 | 30 | ultrafast | cavlc | BYTE-EXACT | 39.31 | 39.31 | 0 |
| 042 | warzone2020 | 30 | ultrafast | cabac | BYTE-EXACT | 39.31 | 39.31 | 0 |
| 043 | warzone2020 | 30 | superfast | cavlc | NAL COUNT 34/32 | 28.17 | 21.09 | 0 |
| 044 | warzone2020 | 30 | superfast | cabac | NAL COUNT 34/32 | 28.17 | 9.47 | 1792 |
| 045 | warzone2020 | 30 | veryfast | cavlc | NAL COUNT 36/32 | 28.06 | 21.09 | 0 |
| 046 | warzone2020 | 30 | veryfast | cabac | NAL COUNT 36/32 | 28.06 | 9.47 | 1792 |
| 047 | warzone2020 | 30 | faster | cavlc | NAL COUNT 36/32 | 28.06 | 20.6 | 0 |
| 048 | warzone2020 | 30 | faster | cabac | NAL COUNT 36/32 | 28.06 | 9.44 | 1728 |
| 049 | warzone2020 | 30 | fast | cavlc | NAL COUNT 36/32 | 28.08 | 20.6 | 0 |
| 050 | warzone2020 | 30 | fast | cabac | NAL COUNT 36/32 | 28.04 | 9.45 | 1728 |
| 051 | warzone2020 | 30 | medium | cavlc | NAL COUNT 36/32 | 28.06 | 20.78 | 0 |
| 052 | warzone2020 | 30 | medium | cabac | NAL COUNT 36/32 | 28.03 | 9.48 | 1920 |
| 053 | warzone2020 | 30 | slow | cavlc | NAL COUNT 36/32 | 28.02 | 20.78 | 0 |
| 054 | warzone2020 | 30 | slow | cabac | NAL COUNT 36/32 | 28.04 | 9.46 | 1723 |
| 055 | warzone2020 | 30 | slower | cavlc | NAL2 byte10 | 29.13 | 20.78 | 0 |
| 056 | warzone2020 | 30 | slower | cabac | NAL2 byte5 | 29.14 | 9.5 | 1782 |
| 057 | warzone2020 | 30 | veryslow | cavlc | NAL2 byte10 | 35.92 | - | - |
| 058 | warzone2020 | 30 | veryslow | cabac | NAL2 byte6 | 35.89 | 9.67 | 1406 |
| 059 | warzone2020 | 30 | placebo | cavlc | NAL2 byte10 | 22.84 | - | - |
| 060 | warzone2020 | 30 | placebo | cabac | NAL2 byte6 | 22.85 | - | - |
| 061 | forza | 2 | ultrafast | cavlc | BYTE-EXACT | 43.05 | 43.05 | 0 |
| 062 | forza | 2 | ultrafast | cabac | BYTE-EXACT | 43.05 | 43.05 | 0 |
| 063 | forza | 2 | superfast | cavlc | NAL2 byte4 | -1 | -1 | -1 |
| 064 | forza | 2 | superfast | cabac | NAL2 byte6 | -1 | -1 | -1 |
| 065 | forza | 2 | veryfast | cavlc | NAL2 byte4 | -1 | -1 | -1 |
| 066 | forza | 2 | veryfast | cabac | NAL2 byte6 | -1 | -1 | -1 |
| 067 | forza | 2 | faster | cavlc | NAL2 byte4 | -1 | -1 | -1 |
| 068 | forza | 2 | faster | cabac | NAL2 byte6 | -1 | - | - |
| 069 | forza | 2 | fast | cavlc | NAL2 byte4 | -1 | -1 | -1 |
| 070 | forza | 2 | fast | cabac | NAL2 byte5 | -1 | - | - |
| 071 | forza | 2 | medium | cavlc | NAL2 byte4 | -1 | -1 | -1 |
| 072 | forza | 2 | medium | cabac | NAL2 byte5 | -1 | -1 | -1 |
| 073 | forza | 2 | slow | cavlc | NAL2 byte4 | -1 | -1 | -1 |
| 074 | forza | 2 | slow | cabac | NAL2 byte5 | -1 | -1 | -1 |
| 075 | forza | 2 | slower | cavlc | NAL2 byte4 | -1 | -1 | -1 |
| 076 | forza | 2 | slower | cabac | NAL2 byte5 | -1 | - | - |
| 077 | forza | 2 | veryslow | cavlc | NAL2 byte5 | -1 | - | - |
| 078 | forza | 2 | veryslow | cabac | NAL2 byte6 | -1 | -1 | -1 |
| 079 | forza | 2 | placebo | cavlc | NAL2 byte5 | -1 | - | - |
| 080 | forza | 2 | placebo | cabac | NAL2 byte6 | -1 | - | - |
| 081 | forza | 8 | ultrafast | cavlc | BYTE-EXACT | 41.55 | 41.55 | 0 |
| 082 | forza | 8 | ultrafast | cabac | BYTE-EXACT | 41.55 | 41.55 | 0 |
| 083 | forza | 8 | superfast | cavlc | NAL COUNT 12/10 | 25.37 | 27.17 | 0 |
| 084 | forza | 8 | superfast | cabac | NAL COUNT 12/10 | 25.37 | 3.69 | 192 |
| 085 | forza | 8 | veryfast | cavlc | NAL2 byte4 | 24.36 | 27.17 | 0 |
| 086 | forza | 8 | veryfast | cabac | NAL2 byte6 | 24.36 | 3.69 | 192 |
| 087 | forza | 8 | faster | cavlc | NAL2 byte4 | 24.34 | 26.88 | 0 |
| 088 | forza | 8 | faster | cabac | NAL2 byte6 | 24.33 | - | - |
| 089 | forza | 8 | fast | cavlc | NAL2 byte4 | 24.33 | 27.09 | 0 |
| 090 | forza | 8 | fast | cabac | NAL2 byte5 | 24.32 | - | - |
| 091 | forza | 8 | medium | cavlc | NAL2 byte4 | 24.32 | 27.16 | 0 |
| 092 | forza | 8 | medium | cabac | NAL2 byte5 | 24.32 | 3.72 | 312 |
| 093 | forza | 8 | slow | cavlc | NAL2 byte4 | 24.33 | 27.16 | 0 |
| 094 | forza | 8 | slow | cabac | NAL2 byte5 | 24.32 | 3.71 | 381 |
| 095 | forza | 8 | slower | cavlc | NAL2 byte4 | 24.99 | 27.16 | 0 |
| 096 | forza | 8 | slower | cabac | NAL2 byte5 | 25.02 | - | - |
| 097 | forza | 8 | veryslow | cavlc | NAL2 byte5 | 24.88 | - | - |
| 098 | forza | 8 | veryslow | cabac | NAL2 byte6 | 24.86 | 3.7 | 254 |
| 099 | forza | 8 | placebo | cavlc | NAL2 byte5 | 24.87 | 4.53 | 320 |
| 100 | forza | 8 | placebo | cabac | NAL2 byte6 | 24.84 | - | - |
| 101 | forza | 30 | ultrafast | cavlc | BYTE-EXACT | 42.07 | 42.07 | 0 |
| 102 | forza | 30 | ultrafast | cabac | BYTE-EXACT | 42.07 | 42.07 | 0 |
| 103 | forza | 30 | superfast | cavlc | NAL COUNT 40/32 | 31.08 | 23.24 | 0 |
| 104 | forza | 30 | superfast | cabac | NAL COUNT 40/32 | 31.08 | 3.64 | 1728 |
| 105 | forza | 30 | veryfast | cavlc | NAL COUNT 34/32 | 25.5 | 23.24 | 0 |
| 106 | forza | 30 | veryfast | cabac | NAL COUNT 34/32 | 25.49 | 3.64 | 1728 |
| 107 | forza | 30 | faster | cavlc | NAL COUNT 34/32 | 25.48 | 24.79 | 0 |
| 108 | forza | 30 | faster | cabac | NAL COUNT 34/32 | 25.46 | 3.61 | 1600 |
| 109 | forza | 30 | fast | cavlc | NAL COUNT 34/32 | 25.48 | 24.88 | 0 |
| 110 | forza | 30 | fast | cabac | NAL COUNT 34/32 | 25.48 | 3.6 | 1600 |
| 111 | forza | 30 | medium | cavlc | NAL COUNT 34/32 | 25.48 | 24.87 | 0 |
| 112 | forza | 30 | medium | cabac | NAL COUNT 34/32 | 25.47 | 3.65 | 1784 |
| 113 | forza | 30 | slow | cavlc | NAL COUNT 34/32 | 25.47 | 24.87 | 0 |
| 114 | forza | 30 | slow | cabac | NAL COUNT 34/32 | 25.46 | 3.61 | 1597 |
| 115 | forza | 30 | slower | cavlc | NAL2 byte4 | 32.03 | 24.87 | 0 |
| 116 | forza | 30 | slower | cabac | NAL2 byte5 | 32.07 | 3.78 | 1660 |
| 117 | forza | 30 | veryslow | cavlc | NAL2 byte5 | 27.0 | - | - |
| 118 | forza | 30 | veryslow | cabac | NAL2 byte6 | 26.99 | 3.62 | 1662 |
| 119 | forza | 30 | placebo | cavlc | NAL2 byte5 | 27.02 | 4.06 | 1600 |
| 120 | forza | 30 | placebo | cabac | NAL2 byte6 | 27.0 | 3.69 | 1551 |
