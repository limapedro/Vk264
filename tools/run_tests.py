#!/usr/bin/env python3
"""Configure, build and run all Phoenix unit tests with CMake.

Usage: python tools/run_tests.py
"""
import pathlib, subprocess, sys, os

R=pathlib.Path(__file__).resolve().parents[1]
B=R/'build'/'tests'
cmd=['cmake','-S',str(R),'-B',str(B)]
if os.name=='nt':
    cmd += ['-A','x64']
r=subprocess.run(cmd,capture_output=True,text=True,timeout=600)
if r.returncode:
    print(r.stdout[-2000:]); print(r.stderr[-2000:]); sys.exit(r.returncode)

r=subprocess.run(['cmake','--build',str(B),'--config','Release'],
                 capture_output=True,text=True,timeout=600)
if r.returncode:
    print(r.stdout[-2000:]); print(r.stderr[-2000:]); sys.exit(r.returncode)

r=subprocess.run(['ctest','--test-dir',str(B),'-C','Release','--output-on-failure'],
                 capture_output=True,text=True,timeout=600)
print(r.stdout.strip())
sys.exit(r.returncode)
