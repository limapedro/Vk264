import sys, pathlib
sys.path.insert(0, 'Vk264-rewrite/tools')
from trace_real_dc import quant, shift, Q4S, div

data = pathlib.Path(sys.argv[1]).read_bytes()
w = int(sys.argv[2]); h = int(sys.argv[3])
ysize = w*h
u = data[ysize:ysize+ysize//4]
v = data[ysize+ysize//4:ysize+ysize//2]
qp = int(sys.argv[4])
q,s = qp%6, qp//6
j = shift(Q4S[q][0], s-1)
# chroma list uses deadzone 21, same quant4_scale base
mf = j>>1
bias = min(div(21<<10, j), (1<<15)//j)<<1
print('mf',mf,'bias',bias)

def dct4(diff):
    t=[0]*16
    for i in range(4):
        s03=diff[i*4]+diff[i*4+3]; s12=diff[i*4+1]+diff[i*4+2]
        d03=diff[i*4]-diff[i*4+3]; d12=diff[i*4+1]-diff[i*4+2]
        t[i]=s03+s12; t[4+i]=2*d03+d12; t[8+i]=s03-s12; t[12+i]=d03-2*d12
    o=[0]*16
    for i in range(4):
        s03=t[i*4]+t[i*4+3]; s12=t[i*4+1]+t[i*4+2]
        d03=t[i*4]-t[i*4+3]; d12=t[i*4+1]-t[i*4+2]
        o[i*4]=s03+s12; o[i*4+1]=2*d03+d12; o[i*4+2]=s03-s12; o[i*4+3]=d03-2*d12
    return o

for name,plane in [('Cb',u),('Cr',v)]:
    dcs=[]
    for b in range(4):
        by=(b//2)*4; bx=(b%2)*4
        diff=[plane[(by+y)*8+bx+x]-128 for y in range(4) for x in range(4)]
        dcs.append(dct4(diff)[0])
    a=dcs[0]+dcs[1]; bb=dcs[2]+dcs[3]; c=dcs[0]-dcs[1]; d=dcs[2]-dcs[3]
    had=[a+bb,a-bb,c+d,c-d]
    qq=[quant(x,mf,bias) for x in had]
    print(name,'dcs',dcs,'had',had,'quant',qq)
