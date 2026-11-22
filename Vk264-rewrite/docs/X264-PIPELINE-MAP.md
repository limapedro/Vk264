# x264 encode pipeline map (research reference)

Source: `references/x264-master` (checked-out C-only build, `config.h` has all
`HAVE_*` SIMD macros set to 0). Line numbers are from that tree.

## Top-level chain

```
x264_encoder_encode                     encoder/encoder.c:3323
 -> slice_init                          encoder/encoder.c:3873 / 2608
 -> slices_write                        encoder/encoder.c:3898 / 3159
   -> slice_write                       encoder/encoder.c:3202 / 2752
     -> slice_header_write              encoder/encoder.c:2791 / 213
     -> x264_macroblock_cache_load_progressive
                                        common/macroblock.c:1354
     -> x264_macroblock_analyse         encoder/analyse.c:2918
     -> x264_macroblock_encode          encoder/macroblock.c:974
       -> macroblock_encode_internal    encoder/macroblock.c:618
         -> mb_encode_i16x16            encoder/macroblock.c:126
         -> x264_mb_encode_chroma       encoder/macroblock.c:492
     -> x264_macroblock_write_cavlc     encoder/cavlc.c:487
       -> cavlc_mb_header_i             encoder/cavlc.c:305
       -> cavlc_qp_delta                encoder/cavlc.c:211
       -> cavlc_block_residual_internal encoder/cavlc.c:121
     -> x264_macroblock_cache_save      common/macroblock.c:1680
     -> x264_macroblock_deblock_strength common/macroblock.c:1438
 -> encoder_frame_end                   encoder/encoder.c:3904
   -> encoder_encapsulate_nals          encoder/encoder.c:2051
     -> x264_nal_encode                 common/bitstream.c:55
```

SPS/PPS: `x264_encoder_headers` (encoder.c:2096) calls `x264_sps_write`
(encoder/set.c:305) and `x264_pps_write` (encoder/set.c:505); initialisation is
`x264_sps_init` (set.c:101) and `x264_pps_init` (set.c:479).

## I_16x16 CAVLC writes (encoder/cavlc.c)

| Syntax | Location |
|---|---|
| mb_type (combined with pred mode + CBP) | cavlc.c:308-312 via 543 |
| chroma pred mode | cavlc.c:333-334 |
| mb_qp_delta | cavlc.c:211-239 via 560 |
| luma DC residual | cavlc.c:563-566 |
| luma AC residual (16 blocks) | cavlc.c:568-570 |
| chroma DC residual | cavlc.c:580-582 |
| chroma AC residual | cavlc.c:583-589 |

I-slice `cavlc_mb_header_i(h, type, 0, chroma)`; P-slice uses offset 5.

## C fallbacks (non-asm)

- 16x16 prediction: `x264_predict_16x16_{v,h,dc,p}_c` — common/predict.c:116,104,67,132
- chroma prediction: `x264_predict_8x8c_{v,h,dc,p}_c` — predict.c:270,260,221,282
- 4x4 DCT: `sub4x4_dct` common/dct.c:157; 16x wrapper `sub16x16_dct` dct.c:199
- luma16 DC Hadamard: `dct4x4dc` dct.c:47
- chroma DC: `sub8x8_dct_dc` dct.c:216
- quant: `quant_4x4x4` common/quant.c:75, `quant_4x4_dc` quant.c:88,
  `quant_2x2_dc` quant.c:96
- zigzag: `zigzag_scan_4x4_frame` dct.c:843, `zigzag_scan_2x2_dc`
  encoder/macroblock.c:35
- CAVLC coefficient coding: `cavlc_block_residual_internal` cavlc.c:121
- deblock: `deblock_v_luma_c` common/deblock.c:128, `deblock_h_luma_c`:132,
  `deblock_v_chroma_c`:170, `deblock_h_chroma_c`:174

## Bitstream escape

`nal_escape_c` common/bitstream.c:29-40 inserts 0x03 when
`src[0] <= 0x03 && !dst[-2] && !dst[-1]`. `x264_nal_encode` writes the
Annex-B start code (4 bytes for first/SPS/PPS) at bitstream.c:61-68, then the
NAL header byte at :73.

## Minimal C-only source set (Makefile:20-30)

common/{osdep,base,cpu,tables}.c encoder/api.c plus
common/{mc,predict,pixel,macroblock,frame,dct,cabac,common,rectangle,set,quant,
deblock,vlc,mvpred,bitstream}.c and
encoder/{analyse,me,ratecontrol,set,macroblock,cabac,cavlc,encoder,lookahead}.c.
`--disable-asm` keeps the C function-table defaults.

## Structures

x264_param_t x264.h:310; x264_picture_t x264.h:862; x264_sps_t common/set.h:60;
x264_pps_t common/set.h:150; x264_t common/common.h:270; slice header
common/common.h:153; MB cache common/common.h:519-651.
