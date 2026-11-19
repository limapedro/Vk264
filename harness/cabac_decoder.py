"""Syntax-aware CABAC decoder for x264 I4x4 slices, driven by x264-master tables.

Parses references/x264-master/common/tables.c at runtime (x264 is authoritative),
then decodes the macroblock header syntax in x264's write order:
  encoder/cabac.c::cabac_mb_header_i + macroblock_write_cabac_internal.

Usage: python harness/cabac_decoder.py <slice-hex> [qp]
prints decoded mb_type / transform_size / 16 intra4x4 modes / chroma pred mode /
cbp_luma / cbp_chroma / qp_delta, then the remaining raw bins.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TEXT = (ROOT / "references" / "x264-master" / "common" / "tables.c").read_text()


def parse_2d(name):
    m = re.search(re.escape(name) + r"\[(\d+)\]\[(\d+)\]\s*=\s*\{(.*?)\};", TEXT, re.S)
    if not m:
        raise SystemExit(f"missing table {name}")
    body = re.sub(r"/\*.*?\*/", "", m.group(3), flags=re.S)
    nums = [int(x) for x in re.findall(r"-?\d+", body)]
    rows, cols = int(m.group(1)), int(m.group(2))
    assert len(nums) == rows * cols, (name, len(nums), rows * cols)
    return [nums[i * cols:(i + 1) * cols] for i in range(rows)]


INIT_I = parse_2d("x264_cabac_context_init_I")
LPS = parse_2d("x264_cabac_range_lps")
TRANS = parse_2d("x264_cabac_transition")
FIX = [-1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 2, 2, 2]  # x264_mb_pred_mode4x4_fix[t+1]


def clamp(v, lo, hi):
    return max(lo, min(hi, v))


class Decoder:
    def __init__(self, data, qp):
        self.data, self.pos, self.bits, self.buf = data, 0, 0, 0
        self.state = [clamp((m * qp >> 4) + n, 1, 126) for m, n in INIT_I]
        self.range = 0x1FE
        self.code = 0
        for _ in range(9):
            self.code = (self.code << 1) | self.read_bit()
        self.bins = []

    def read_bit(self):
        if self.bits == 0:
            self.buf = self.data[self.pos] if self.pos < len(self.data) else 0
            self.pos += 1
            self.bits = 8
        self.bits -= 1
        return (self.buf >> self.bits) & 1

    def bypass(self):
        self.code <<= 1
        bit = self.read_bit()
        self.code |= bit
        if self.code >= self.range:
            self.code -= self.range
            return 0
        return 1

    def decision(self, ctx):
        st = self.state[ctx]
        idx = (self.range >> 6) - 4
        if not 0 <= idx < 4:
            raise RuntimeError(f"bad range {self.range} at bin {len(self.bins)} ctx {ctx}")
        lps = LPS[st >> 1][idx]
        self.range -= lps
        if self.code >= self.range:
            self.code -= self.range
            self.range = lps
            bit = 1 - (st & 1)
        else:
            bit = st & 1
        self.state[ctx] = TRANS[st][bit]
        while self.range < 256:
            self.range <<= 1
            self.code = (self.code << 1) | self.read_bit()
        self.bins.append((len(self.bins), ctx, bit))
        return bit


def parse(data, qp):
    d = Decoder(data, qp)
    mb0 = d.decision(3)                 # I-slice mb_type first bin (0 => I4x4)
    ts0 = d.decision(399)               # transform_size (8x8dct enabled)
    modes = []
    for i in range(16):
        x, y = i % 4, i // 4
        left = FIX[modes[i - 1]] if x else -1
        top = FIX[modes[i - 4]] if y else -1
        pred = min(left, top)
        if pred < 0:
            pred = 2
        if d.decision(68):
            mode = pred
        else:
            m = d.decision(69) | (d.decision(69) << 1) | (d.decision(69) << 2)
            mode = m if m < pred else m + 1
        modes.append(mode)
    chroma = d.decision(64)
    if chroma:
        chroma |= d.decision(67) << 1
        if chroma > 1:
            chroma |= d.decision(67) << 2
    cl, ct = -1, -1
    cbp = 0
    for b in range(4):
        # Follow x264 exactly:
        #   bit0: 76 - ((cl>>1)&1) - ((ct>>1)&2)
        #   bit1: 76 - ((cbp>>0)&1) - ((ct>>2)&2)
        #   bit2: 76 - ((cl>>3)&1) - ((cbp<<1)&2)
        #   bit3: 76 - ((cbp>>2)&1) - ((cbp>>0)&2)
        if b == 0: ctx = 76 - ((cl >> 1) & 1) - ((ct >> 1) & 2)
        elif b == 1: ctx = 76 - ((cbp >> 0) & 1) - ((ct >> 2) & 2)
        elif b == 2: ctx = 76 - ((cl >> 3) & 1) - ((cbp << 1) & 2)
        else: ctx = 76 - ((cbp >> 2) & 1) - ((cbp >> 0) & 2)
        cbp |= d.decision(ctx) << b
    cbp_a, cbp_b = cl & 0x30, ct & 0x30
    cbp_c_ctx = 0
    if cbp_a and cl != -1: cbp_c_ctx += 1
    if cbp_b and ct != -1: cbp_c_ctx += 2
    cbp_chroma = d.decision(77 + cbp_c_ctx)
    if cbp_chroma:
        c2 = 4 + (1 if cbp_a == 0x20 else 0) + (2 if cbp_b == 0x20 else 0)
        cbp_chroma |= d.decision(77 + c2) << 1
    # qp_delta: first bin; 1 means a nonzero delta follows
    dqp0 = d.decision(60)
    # residual coding (x264 DCT_LUMA_4x4 = category 2)
    ctx_sig, ctx_last, ctx_level = 134, 195, 247
    count_m1 = 15
    level1 = [1, 2, 3, 4, 0, 0, 0, 0]
    levelgt1 = [5, 5, 5, 5, 6, 7, 8, 9]
    trans = [[1, 2, 3, 3, 4, 5, 6, 7], [4, 4, 4, 4, 5, 6, 7, 7]]
    nnz = [0] * 16
    coeffs = {}
    for group in range(4):
        if not (cbp & (1 << group)):
            continue
        for j in range(4):
            idx = group * 4 + j
            x, y = idx % 4, idx // 4
            left_nz = nnz[idx - 1] if x else 0
            top_nz = nnz[idx - 4] if y else 0
            cbf_ctx = 93 + 2 * (1 if top_nz else 0) + (1 if left_nz else 0)
            cbf = d.decision(cbf_ctx)
            if not cbf:
                continue
            sig = []
            last = count_m1 - 1
            for i in range(count_m1):
                if d.decision(ctx_sig + i):
                    sig.append(i)
                    if d.decision(ctx_last + i):
                        last = i
                        break
                if i == count_m1 - 1:
                    sig.append(i)
            coeff_vals = []
            node = 0
            for _ in range(len(sig) - 1, -1, -1):
                c = level1[node] + ctx_level
                if d.decision(c):
                    c = levelgt1[node] + ctx_level
                    ones = 0
                    while d.decision(c):
                        ones += 1
                    absval = ones + 2
                    node = trans[1][node]
                else:
                    absval = 1
                    node = trans[0][node]
                coeff_vals.append(absval)
                d.bypass()  # sign (not needed for nnz/level comparison)
            nnz[idx] = len(sig)
            coeffs[idx] = (sig, coeff_vals)
    return dict(mb0=mb0, ts0=ts0, modes=modes, chroma=chroma,
                cbp=cbp, cbp_chroma=cbp_chroma, dqp0=dqp0,
                nnz=nnz, coeffs=coeffs), d


def main():
    data = bytes.fromhex(sys.argv[1].replace(" ", ""))
    qp = int(sys.argv[2]) if len(sys.argv) > 2 else 25
    info, d = parse(data, qp)
    print(f"mb0={info['mb0']} ts0={info['ts0']}")
    print("modes:", [info["modes"][4 * y:4 * y + 4] for y in range(4)])
    print(f"chroma_mode={info['chroma']} cbp_luma={info['cbp']:#x} "
          f"cbp_chroma={info['cbp_chroma']} dqp0={info['dqp0']}")
    print(f"nnz per 4x4 block: {info['nnz']}")
    for idx, (sig, vals) in info["coeffs"].items():
        print(f"  block {idx}: sig={sig} abslevels={vals}")
    print(f"bins ({len(d.bins)}): " + " ".join(f"{c}:{b}" for _, c, b in d.bins))


if __name__ == "__main__":
    main()
