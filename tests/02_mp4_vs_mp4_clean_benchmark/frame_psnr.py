import pathlib, math
exp=pathlib.Path('tests/02_mp4_vs_mp4_clean_benchmark')
W,H=1920,1080; FS=W*H*3//2; YN=W*H
src=(exp/'source.yuv').read_bytes()
vk=(exp/'vk_dec.yuv').read_bytes()
x=(exp/'x_dec.yuv').read_bytes()
n=min(len(src),len(vk),len(x))//FS
print('frames',n)
def psnr(a,b):
    mse=sum((p-q)**2 for p,q in zip(a,b))/len(a)
    return 10*math.log10(255*255/mse) if mse else float('inf')
for i in [0,1,2,3,5,10,30,59,60,61,90,119]:
    if i>=n: continue
    s=src[i*FS:i*FS+YN]; v=vk[i*FS:i*FS+YN]; xx=x[i*FS:i*FS+YN]
    print(f'frame {i:3d}: vk={psnr(s,v):6.2f}dB x264={psnr(s,xx):6.2f}dB vk_vs_x={psnr(v,xx):6.2f}dB')
