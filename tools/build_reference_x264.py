#!/usr/bin/env python3
"""Build an 8-bit C-only x264 reference from reference/x264/ with MSVC.

The authoritative configuration is copied from the exact reference build
(reference/x264/config.h). Assembly and optional external libraries are
disabled; x264's C implementations are bit-identical to its asm ones. The
result is written to build/x264_ref/x264.exe and auto-detected by
tools/parity_harness.py.
"""
import pathlib, shutil, subprocess, sys

R = pathlib.Path(__file__).resolve().parents[1]
SRC = R / 'reference/x264'
OUT = R / 'build' / 'x264_ref'
VC = pathlib.Path(r'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat')

SRCS = [
    'common/osdep.c', 'common/base.c', 'common/cpu.c', 'common/tables.c',
    'encoder/api.c', 'common/mc.c', 'common/predict.c', 'common/pixel.c',
    'common/macroblock.c', 'common/frame.c', 'common/dct.c', 'common/cabac.c',
    'common/common.c', 'common/rectangle.c', 'common/set.c', 'common/quant.c',
    'common/deblock.c', 'common/vlc.c', 'common/mvpred.c', 'common/bitstream.c',
    'encoder/analyse.c', 'encoder/me.c', 'encoder/ratecontrol.c',
    'encoder/set.c', 'encoder/macroblock.c', 'encoder/cabac.c',
    'encoder/cavlc.c', 'encoder/encoder.c', 'encoder/lookahead.c',
    'x264.c', 'autocomplete.c', 'input/input.c', 'input/timecode.c',
    'input/raw.c', 'input/y4m.c', 'output/raw.c', 'output/matroska.c',
    'output/matroska_ebml.c', 'output/flv.c', 'output/flv_bytestream.c',
    'filters/filters.c', 'filters/video/video.c', 'filters/video/source.c',
    'filters/video/internal.c', 'filters/video/resize.c',
    'filters/video/fix_vfr_pts.c', 'filters/video/select_every.c',
    'filters/video/crop.c', 'filters/video/cache.c', 'filters/video/depth.c',
    'input/thread.c', 'common/win32thread.c', 'common/threadpool.c',
    'extras/getopt.c',
]


def main():
    if not VC.exists():
        print('SKIP: MSVC vcvars64.bat not found'); return 2
    OBJ = OUT / 'obj'
    shutil.rmtree(OBJ, ignore_errors=True)
    OBJ.mkdir(parents=True, exist_ok=True)
    inc = ' '.join('/I"' + str(SRC/d) + '"' for d in
                   ['.', 'common', 'encoder', 'input', 'output', 'filters',
                    'filters/video', 'extras'])
    defs = '/DBIT_DEPTH=8 "/Dx264_template(w)=x264_glue3(x264,8,w)" ' \
           '/D__STDC_CONSTANT_MACROS /DHAVE_CONFIG_H'
    for s in SRCS:
        obj = OBJ / (s.replace('/', '_').replace('.c', '.obj'))
        cmd = f'"{VC}" && cl -nologo -O2 -c {defs} {inc} "{SRC/s}" /Fo"{obj}"'
        r = subprocess.run(cmd, shell=True, capture_output=True, text=True,
                           cwd=str(SRC), timeout=180)
        if r.returncode:
            print('FAIL', s, (r.stdout+r.stderr)[-800:]); return 1
    rsp = OUT / 'objs.rsp'
    rsp.write_text('\n'.join('"' + str(o) + '"' for o in sorted(OBJ.glob('*.obj')))
                   + '\nshell32.lib\n', encoding='utf-8')
    cmd = f'"{VC}" && link -nologo /OUT:"{OUT/"x264.exe"}" @"{rsp}"'
    r = subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=300)
    if r.returncode:
        print((r.stdout+r.stderr)[-1000:]); return 1
    print('built', OUT / 'x264.exe')
    return 0


if __name__ == '__main__':
    sys.exit(main())
