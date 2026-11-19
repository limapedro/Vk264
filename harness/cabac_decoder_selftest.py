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
        self.state = [max(1, min(126, (m * qp >> 4) + n)) for m, n in cd.INIT_I]
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
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
