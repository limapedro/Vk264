#!/usr/bin/env python3
"""Build and run the Phoenix unit tests with MSVC (vcvars64.bat).

Usage: python vk264-phoenix/tools/run_tests.py
"""
import pathlib, subprocess, sys, os
R=pathlib.Path(__file__).resolve().parents[1]
VC=r'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat'
if not pathlib.Path(VC).exists():
    print('vcvars64.bat not found:', VC); sys.exit(2)
srcs=[str(R/'src/phx_param.c'),str(R/'tests/test_param.c')]
out=R/'tests/test_param.exe'
cmd=f'"{VC}" && cl -nologo -Od -W3 -I"{R/"src"}" '+' '.join(srcs)+f' /Fe"{out}"'
r=subprocess.run(cmd,shell=True,capture_output=True,text=True,timeout=300)
if r.returncode:
    print(r.stdout[-2000:]); print(r.stderr[-1000:]); sys.exit(r.returncode)
r=subprocess.run([str(out)],capture_output=True,text=True,timeout=120)
print(r.stdout.strip())
sys.exit(r.returncode)
