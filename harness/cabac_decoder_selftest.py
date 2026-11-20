"""Round-trip self-test for harness/cabac_decoder.py against an independent encoder.

The encoder below is a direct transcription of x264's CABAC primitives
(common/cabac.c): decision, bypass and flush. It is deliberately separate from
the decoder under test. Run:

    python harness/cabac_decoder_selftest.py
"""

import random
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import cabac_decoder as cd


class ReferenceEncoder:
    def __init__(self, qp):
        # Match x264's stored state encoding: (min(s,127-s)<<1)|(s>>6).
        self.state = [((min(s, 127 - s) << 1) | (s >> 6))
                      for m, n in cd.INIT_I
                      for s in [max(1, min(126, (m * qp >> 4) + n))]]
        self.low = 0
        self.range = 0x1FE
        self.queue = -9
        self.outstanding = 0
        self.out = []

    def put_byte(self):
        if self.queue < 0:
            return
        out = self.low >> (self.queue + 10)
        self.low &= (0x400 << self.queue) - 1
        self.queue -= 8
        if (out & 255) == 255:
            self.outstanding += 1
            return
        carry = out >> 8
        if carry:
            self.out[-1] = (self.out[-1] + carry) & 255
        while self.outstanding:
            self.out.append((carry - 1) & 255)
            self.outstanding -= 1
        self.out.append(out & 255)

    def renormalize(self):
        while self.range < 256:
            self.range <<= 1
            self.low <<= 1
            self.queue += 1
            self.put_byte()

    def decision(self, ctx, bit):
        st = self.state[ctx]
        lps = cd.LPS[st >> 1][(self.range >> 6) - 4]
        self.range -= lps
        if bit != (st & 1):
            self.low += self.range
            self.range = lps
        self.state[ctx] = cd.TRANS[st][bit]
        self.renormalize()

    def flush(self, frame):
        self.low += self.range - 2
        self.low |= 1
        self.low <<= 9
        self.queue += 9
        self.put_byte()
        self.put_byte()
        self.low <<= -self.queue
        self.low |= ((0x35A4E4F5 >> (frame & 31)) & 1) << 10
        self.queue = 0
        self.put_byte()
        while self.outstanding:
            self.out.append(255)
            self.outstanding -= 1


def main():
    random.seed(7)
    trials = 0
    for qp in (0, 25, 51):
        for _ in range(20):
            enc = ReferenceEncoder(qp)
            seq = [(random.randrange(460), random.randrange(2)) for _ in range(60)]
            for ctx, bit in seq:
                enc.decision(ctx, bit)
            enc.flush(0)
            dec = cd.Decoder(enc.out, qp)
            for ctx, bit in seq:
                if dec.decision(ctx) != bit:
                    raise SystemExit(f"round-trip mismatch at qp={qp}")
            trials += 1
    print(f"cabac_decoder round-trip OK: {trials} streams, QP 0/25/51.")

    # Syntax-oracle regression: decode the byte-exact x264 superfast/flat slice
    # (PR #41/#45) and require its I4x4 header to match the captured ground truth.
    # The input is the CABAC payload only (bytes after the 88 84 3f slice header).
    x264_slice_cabac = bytes.fromhex("ea4035709bfe0b72bf3e1ac9b134b43181")
    info, d = cd.parse(x264_slice_cabac, 25)
    expected_modes = [2, 2, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2]
    for key, want in (("mb0", 0), ("ts0", 0), ("modes", expected_modes),
                      ("chroma", 0), ("cbp", 1), ("cbp_chroma", 1), ("dqp", 0)):
        if info[key] != want:
            raise SystemExit(f"x264 I4x4 header mismatch on {key}: {info[key]} != {want}")
    first_bins = [(c, b) for _, c, b in d.bins[:4]]
    if first_bins != [(3, 0), (399, 0), (68, 1), (68, 1)]:
        raise SystemExit(f"x264 I4x4 leading bins mismatch: {first_bins}")
    print("cabac_decoder x264 I4x4 slice header matches byte-exact ground truth.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
