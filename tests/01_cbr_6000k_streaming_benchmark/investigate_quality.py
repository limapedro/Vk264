"""Investigate why x264_cbr6000.mp4 looks better than vk264_cbr6000.h264.
Decode both to YUV, compare frame 0 (IDR) and later P-frames separately."""
import subprocess, pathlib, re
exp = pathlib.Path('tests/01_cbr_6000k_streaming_benchmark')
root = pathlib.Path('.')
W,H,N = 1920,1080,660

def decode(path, out, rate=None):
    cmd = ['ffmpeg','-v','error','-y']
    if rate: cmd += ['-r',str(rate)]
    cmd += ['-i',str(path),'-pix_fmt','yuv420p','-f','rawvideo',str(out)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    return r.returncode, r.stderr[:300]

print('=== decode both ===')
xok, xerr = decode(exp/'x264_cbr6000.mp4', exp/'x264_decoded.yuv')
print('x264 mp4  :', 'OK' if xok==0 else xerr)
vok, verr = decode(exp/'vk264_cbr6000.h264', exp/'vk264_decoded.yuv', rate=60)
print('vk264 h264:', 'OK' if vok==0 else verr)

x = (exp/'x264_decoded.yuv').read_bytes()
v = (exp/'vk264_decoded.yuv').read_bytes()
fs = W*H*3//2
print('decoded frames: x264', len(x)//fs, 'vk264', len(v)//fs)

for idx in [0,1,2,5,30,60]:
    if (idx+1)*fs > min(len(x),len(v)): continue
    a=x[idx*fs:(idx+1)*fs]; b=v[idx*fs:(idx+1)*fs]
    # luma-only MSE
    la=a[:W*H].astype if False else None
    import array
    la=array.array('B',a[:W*H]); lb=array.array('B',b[:W*H])
    mse=sum((int(p)-int(q))**2 for p,q in zip(la,lb))/len(la)
    import math
    psnr=10*math.log10(255*255/mse) if mse>0 else float('inf')
    print(f'frame {idx:3d}: PSNR(vk vs x264) = {psnr:.2f} dB')
