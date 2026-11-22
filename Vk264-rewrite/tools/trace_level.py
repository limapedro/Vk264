"""Generate x264 level-token bit patterns for small levels to identify the
coefficient encoded in the reference slice."""
import re
from pathlib import Path

def token(bits, size): return (bits, size)

def level_tokens():
    tab = {}
    for i_suffix in range(7):
        arr = {}
        for level in range(-64,64):
            mask = -1 if level < 0 else 0
            abs_level = (level ^ mask) - mask
            code = abs_level*2-mask-2 if abs_level else 0
            nxt = i_suffix
            if (code >> i_suffix) < 14:
                size = (code >> i_suffix) + 1 + i_suffix
                bits = (1<<i_suffix) + (code & ((1<<i_suffix)-1))
            elif i_suffix == 0 and code < 30:
                size, bits = 19, (1<<4)+(code-14)
            elif i_suffix > 0 and (code >> i_suffix) == 14:
                size = 15+i_suffix
                bits = (1<<i_suffix)+(code & ((1<<i_suffix)-1))
            else:
                code -= 15<<i_suffix
                if i_suffix == 0: code -= 15
                size, bits = 28, (1<<12)+code
            if nxt == 0: nxt += 1
            if abs_level > (3 << (nxt-1)) and nxt < 6: nxt += 1
            arr[level+64] = (bits,size,nxt)
        tab[i_suffix] = arr
    return tab

T = level_tokens()
target = '00010100'  # first bits after coeff_token (byte5 low bits + byte6)
# after coeff_token (6 bits) the remaining bits from byte5 bit1 onward were
# actually 000101 00 ...; skip the 6 token bits and look at following bits
bits = '00'+'00000000'+'00000010'+'00100100'  # approximate stream after coeff token

for suffix in [0]:
    for level in range(-20,21):
        v = level
        val_original = v+64
        val = v
        val -= 1  # trailing<3
        val += 64
        if 0 <= val_original < 128:
            b,s,n = T[suffix][val]
            pat = format(b, '0'+str(s)+'b')
            if pat.startswith(bits[:min(len(pat),6)]):
                print('candidate level', level, 'bits', pat, 'size', s)
