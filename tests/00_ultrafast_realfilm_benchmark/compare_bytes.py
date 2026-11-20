import pathlib, re
exp = pathlib.Path('tests/00_ultrafast_realfilm_benchmark')

def strip_sei(d):
    parts = list(re.finditer(b'\x00\x00\x01', d))
    out = b''
    for i, m in enumerate(parts):
        e = m.end()
        end = parts[i+1].start() if i+1 < len(parts) else len(d)
        if e < len(d) and (d[e] & 0x1f) == 6:
            continue
        out += d[m.start():end]
    return out

vk = strip_sei((exp/'vk264_gop1_full.h264').read_bytes())
x  = strip_sei((exp/'x264_gop1_full.h264').read_bytes())
print('vk stripped bytes:', len(vk))
print('x  stripped bytes:', len(x))
n = min(len(vk), len(x))
diff = next((i for i in range(n) if vk[i] != x[i]), None)
if diff is None and len(vk) == len(x):
    print('BYTE-EXACT MATCH (SEI stripped)')
else:
    print('first mismatch at offset', diff, 'vk', hex(vk[diff]) if diff is not None else '-',
          'x', hex(x[diff]) if diff is not None else '-')
    if diff is not None:
        print('vk ctx:', vk[max(0,diff-8):diff+16].hex())
        print('x  ctx:', x[max(0,diff-8):diff+16].hex())
