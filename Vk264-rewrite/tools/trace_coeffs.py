"""Trace I16 DC coefficients for the flat 16x16 test under both predictors."""
import sys

quant4_scale = [
    [13107,8066,5243],[11916,7490,4660],[10082,6554,4194],
    [9362,5825,3647],[8192,5243,3355],[7282,4559,2893]]

def shift(x,s): return x << -s if s <= 0 else (x + (1 << (s-1))) >> s
def div(n,d): return (n + (d>>1)) // d

def tables(qp, deadzone):
    q,s = qp%6, qp//6
    par = [0,1,1,2,0,1,1,2,1,2,2,0,1,2,2,0]  # (i&1)+((i>>2)&1) for i=0..15
    mf=[]; bias=[]
    for i in range(16):
        j = shift(quant4_scale[q][par[i]], s-1)
        mf.append(j)
        b = min(div(deadzone<<10, j) if j else 0, (1<<15)//j if j else 0)
        bias.append(b)
    return mf,bias

def quant(v, mf, bias):
    if not v: return 0
    mag = ((bias+abs(v))*mf)>>16
    return mag if v>0 else -mag

def dct4x4dc(d):
    t=[0]*16
    for i in range(4):
        s01=d[i*4]+d[i*4+1]; a=d[i*4]-d[i*4+1]
        s23=d[i*4+2]+d[i*4+3]; b=d[i*4+2]-d[i*4+3]
        t[0*4+i]=s01+s23; t[1*4+i]=s01-s23; t[2*4+i]=a-b; t[3*4+i]=a+b
    out=[0]*16
    for i in range(4):
        s01=t[i*4]+t[i*4+1]; a=t[i*4]-t[i*4+1]
        s23=t[i*4+2]+t[i*4+3]; b=t[i*4+2]-t[i*4+3]
        out[i*4]=(s01+s23+1)>>1; out[i*4+1]=(s01-s23+1)>>1
        out[i*4+2]=(a-b+1)>>1; out[i*4+3]=(a+b+1)>>1
    return out

for pred_name, ypred, cpred in [("rep16/128",16,128),("rep16/16",16,16),("128/128",128,128)]:
    # luma: 16 blocks, each DC = sum of 16*(src-pred)
    dc4 = [16*(16-ypred) for _ in range(16)]
    d = dct4x4dc(dc4[:])
    mf,bias = tables(25,11)
    q = [quant(v, mf[0]>>1, bias[0]<<1) for v in d]
    nz = [i for i,v in enumerate(q) if v]
    print(f"{pred_name}: luma DC hadamard d[0]={d[0]} quant={q} nonzero={nz}")

    # chroma: 4 blocks of 16 pixels each, DC sums
    cdc = [16*(16-cpred) for _ in range(4)]
    a=cdc[0]+cdc[1]; b=cdc[2]+cdc[3]; c=cdc[0]-cdc[1]; e=cdc[2]-cdc[3]
    had=[a+b,a-b,c+e,c-e]
    mf2,bias2 = tables(25,21)
    qc=[quant(v, mf2[0]>>1, bias2[0]<<1) for v in had]
    print(f"          chroma had={had} quant={qc}")
