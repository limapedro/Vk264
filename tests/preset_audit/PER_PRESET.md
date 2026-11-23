# Per-preset breakdown (x264 vs oracle)

Definition of byte-exact here: the concatenated H.264 NAL payloads (SPS/PPS/slices) are byte-identical. Raw .h264 and MP4 files may differ in padding/container metadata.

| clip | res | frames | preset | entropy | x264 NAL bytes | oracle NAL bytes | ratio | x264 NALs | oracle NALs | NAL-exact pairs |
|---|---|---|---|---|---|---|---|---|---|---|
| small64 | 64x64 | 2 | fast | cabac | 1722 | 619 | 0.359 | 8 | 8 | 0/2 |
| small64 | 64x64 | 8 | fast | cabac | 3722 | 2774 | 0.745 | 20 | 20 | 0/2 |
| small64 | 64x64 | 30 | fast | cabac | 10391 | 9597 | 0.924 | 70 | 64 | 0/2 |
| small64 | 64x64 | 2 | fast | cavlc | 1789 | 811 | 0.453 | 8 | 8 | 0/2 |
| small64 | 64x64 | 8 | fast | cavlc | 4058 | 3824 | 0.942 | 20 | 20 | 0/2 |
| small64 | 64x64 | 30 | fast | cavlc | 11402 | 13304 | 1.167 | 70 | 64 | 0/2 |
| small64 | 64x64 | 2 | faster | cabac | 1753 | 617 | 0.352 | 8 | 8 | 0/2 |
| small64 | 64x64 | 8 | faster | cabac | 3730 | 2772 | 0.743 | 20 | 20 | 0/2 |
| small64 | 64x64 | 30 | faster | cabac | 10406 | 9595 | 0.922 | 70 | 64 | 0/2 |
| small64 | 64x64 | 2 | faster | cavlc | 1795 | 809 | 0.451 | 8 | 8 | 0/2 |
| small64 | 64x64 | 8 | faster | cavlc | 4010 | 3822 | 0.953 | 20 | 20 | 0/2 |
| small64 | 64x64 | 30 | faster | cavlc | 11507 | 13302 | 1.156 | 70 | 64 | 0/2 |
| small64 | 64x64 | 2 | medium | cabac | 1734 | 619 | 0.357 | 8 | 8 | 0/2 |
| small64 | 64x64 | 8 | medium | cabac | 3762 | 2774 | 0.737 | 20 | 20 | 0/2 |
| small64 | 64x64 | 30 | medium | cabac | 10551 | 9597 | 0.91 | 70 | 64 | 0/2 |
| small64 | 64x64 | 2 | medium | cavlc | 1791 | 811 | 0.453 | 8 | 8 | 0/2 |
| small64 | 64x64 | 8 | medium | cavlc | 3993 | 3827 | 0.958 | 20 | 20 | 0/2 |
| small64 | 64x64 | 30 | medium | cavlc | 11311 | 13320 | 1.178 | 70 | 64 | 0/2 |
| small64 | 64x64 | 2 | placebo | cabac | 1742 | 625 | 0.359 | 8 | 8 | 0/2 |
| small64 | 64x64 | 8 | placebo | cabac | 3416 | 2780 | 0.814 | 20 | 20 | 0/2 |
| small64 | 64x64 | 30 | placebo | cabac | 9240 | 9603 | 1.039 | 64 | 64 | 0/2 |
| small64 | 64x64 | 2 | placebo | cavlc | 1805 | 825 | 0.457 | 8 | 8 | 0/2 |
| small64 | 64x64 | 8 | placebo | cavlc | 3684 | 3893 | 1.057 | 20 | 20 | 0/2 |
| small64 | 64x64 | 30 | placebo | cavlc | 9987 | 13581 | 1.36 | 64 | 64 | 0/2 |
| small64 | 64x64 | 2 | slow | cabac | 1727 | 619 | 0.358 | 8 | 8 | 0/2 |
| small64 | 64x64 | 8 | slow | cabac | 3788 | 2774 | 0.732 | 20 | 20 | 0/2 |
| small64 | 64x64 | 30 | slow | cabac | 10663 | 9597 | 0.9 | 70 | 64 | 0/2 |
| small64 | 64x64 | 2 | slow | cavlc | 1784 | 813 | 0.456 | 8 | 8 | 0/2 |
| small64 | 64x64 | 8 | slow | cavlc | 4050 | 3834 | 0.947 | 20 | 20 | 0/2 |
| small64 | 64x64 | 30 | slow | cavlc | 11483 | 13348 | 1.162 | 70 | 64 | 0/2 |
| small64 | 64x64 | 2 | slower | cabac | 1732 | 621 | 0.359 | 8 | 8 | 0/2 |
| small64 | 64x64 | 8 | slower | cabac | 3498 | 2776 | 0.794 | 20 | 20 | 0/2 |
| small64 | 64x64 | 30 | slower | cabac | 9464 | 9599 | 1.014 | 64 | 64 | 0/2 |
| small64 | 64x64 | 2 | slower | cavlc | 1791 | 817 | 0.456 | 8 | 8 | 0/2 |
| small64 | 64x64 | 8 | slower | cavlc | 3771 | 3848 | 1.02 | 20 | 20 | 0/2 |
| small64 | 64x64 | 30 | slower | cavlc | 10147 | 13399 | 1.32 | 64 | 64 | 0/2 |
| small64 | 64x64 | 2 | superfast | cabac | 1801 | 615 | 0.341 | 8 | 8 | 0/2 |
| small64 | 64x64 | 8 | superfast | cabac | 3788 | 2770 | 0.731 | 22 | 20 | 0/2 |
| small64 | 64x64 | 30 | superfast | cabac | 10528 | 9593 | 0.911 | 74 | 64 | 0/2 |
| small64 | 64x64 | 2 | superfast | cavlc | 1837 | 807 | 0.439 | 8 | 8 | 0/2 |
| small64 | 64x64 | 8 | superfast | cavlc | 4031 | 3816 | 0.947 | 22 | 20 | 0/2 |
| small64 | 64x64 | 30 | superfast | cavlc | 11407 | 13286 | 1.165 | 74 | 64 | 0/2 |
| small64 | 64x64 | 2 | ultrafast | cabac | 1691 | 605 | 0.358 | 8 | 8 | 2/2 |
| small64 | 64x64 | 8 | ultrafast | cabac | 3834 | 2748 | 0.717 | 20 | 20 | 2/2 |
| small64 | 64x64 | 30 | ultrafast | cabac | 10613 | 9527 | 0.898 | 64 | 64 | 2/2 |
| small64 | 64x64 | 2 | ultrafast | cavlc | 1763 | 677 | 0.384 | 8 | 8 | 2/2 |
| small64 | 64x64 | 8 | ultrafast | cavlc | 4165 | 3079 | 0.739 | 20 | 20 | 2/2 |
| small64 | 64x64 | 30 | ultrafast | cavlc | 11570 | 10484 | 0.906 | 64 | 64 | 2/2 |
| small64 | 64x64 | 2 | veryfast | cabac | 1749 | 615 | 0.352 | 8 | 8 | 0/2 |
| small64 | 64x64 | 8 | veryfast | cabac | 3675 | 2770 | 0.754 | 20 | 20 | 0/2 |
| small64 | 64x64 | 30 | veryfast | cabac | 10294 | 9593 | 0.932 | 70 | 64 | 0/2 |
| small64 | 64x64 | 2 | veryfast | cavlc | 1791 | 807 | 0.451 | 8 | 8 | 0/2 |
| small64 | 64x64 | 8 | veryfast | cavlc | 3952 | 3816 | 0.966 | 20 | 20 | 0/2 |
| small64 | 64x64 | 30 | veryfast | cavlc | 11200 | 13286 | 1.186 | 70 | 64 | 0/2 |
| small64 | 64x64 | 2 | veryslow | cabac | 1744 | 623 | 0.357 | 8 | 8 | 0/2 |
| small64 | 64x64 | 8 | veryslow | cabac | 3472 | 2778 | 0.8 | 20 | 20 | 0/2 |
| small64 | 64x64 | 30 | veryslow | cabac | 9327 | 9601 | 1.029 | 64 | 64 | 0/2 |
| small64 | 64x64 | 2 | veryslow | cavlc | 1798 | 825 | 0.459 | 8 | 8 | 0/2 |
| small64 | 64x64 | 8 | veryslow | cavlc | 3636 | 3893 | 1.071 | 20 | 20 | 0/2 |
| small64 | 64x64 | 30 | veryslow | cavlc | 10017 | 13581 | 1.356 | 64 | 64 | 0/2 |
