"""Compute I16 DC coefficients for the real crop with x264's formulas."""
import pathlib, sys
sys.path.insert(0, 'Vk264-rewrite/tools')
from c_tables import CT

Q4S=[[13107,8066,5243],[11916,7490,4660],[10082,6554,4194],
     [9362,5825,3647],[8192,5243,3355],[7282,4559,2893]]
def shift(x,s): return x << -s if s<=0 else (x+(1<<(s-1)))>>s
def div(n,d): return (n+(d>>1))//d
def quant(v,mf,bias):
    if not v: return 0
    mag=((bias+abs(v))*mf)>>16
    return mag if v>0 else -mag
def dct4(d):
    t=[0]*16
    for i in range(4):
        s01=d[i*4]+d[i*4+1]; a=d[i*4]-d[i*4+1]
        s23=d[i*4+2]+d[i*4+3]; b=d[i*4+2]-d[i*4+3]
        t[i]=s01+s23; t[4+i]=s01-s23; t[8+i]=a-b; t[12+i]=a+b
    o=[0]*16
    for i in range(4):
        s01=t[i*4]+t[i*4+1]; a=t[i*4]-t[i*4+1]
        s23=t[i*4+2]+t[i*4+3]; b=t[i*4+2]-t[i*4+3]
        o[i*4]=(s01+s23+1)>>1; o[i*4+1]=(s01-s23+1)>>1
        o[i*4+2]=(a-b+1)>>1; o[i*4+3]=(a+b+1)>>1
    return o
zig=[0,1,4,8,5,2,3,6,9,12,13,10,7,11,14,15]
data=pathlib.Path('Vk264-rewrite/build/real16.yuv').read_bytes()
qp=25; q,s=qp%6,qp//6
j=shift(Q4S[q][0],s-1); mf=j>>1
dz=21; bias=min(div(dz<<10,j),(1<<15)//j)<<1
dcs=[]
for b in range(16):
    by=(b//4)*4; bx=(b%4)*4
    diff=[data[(by+y)*16+bx+x]-128 for y in range(4) for x in range(4)]
    d=dct4(diff)
    dcs.append(d[0])
print('block DCs',dcs)
print('mf',mf,'bias',bias)
had=dct4(dcs)
print('hadamard',had)
qdc=[quant(v,mf,bias) for v in had]
print('quant raster',qdc)
print('quant scan  ',[qdc[i] for i in zig])
