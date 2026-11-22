"""Parse static const vk_vlc_t C arrays from the generated header into nested lists."""
import re
from pathlib import Path

def parse_table(header_text: str, name: str):
    m = re.search(r'static const vk_vlc_t '+re.escape(name)+r'(?:\[[^\]]*\])+\s*=\s*\{', header_text)
    if not m: raise KeyError(name)
    s = header_text[m.end()-1:]
    toks = re.findall(r'\{|\}|[0-9]+|,', s)
    pos = 0
    def value():
        nonlocal pos
        if toks[pos] == '{':
            pos += 1
            items = []
            while toks[pos] != '}':
                if toks[pos] == ',': pos += 1; continue
                items.append(value())
            pos += 1
            return items
        v = int(toks[pos]); pos += 1
        return v
    root = value()
    return root

HEADER = (Path(__file__).resolve().parent.parent/'src'/'ref'/'cavlc_tables.h').read_text()
CT = parse_table(HEADER, 'vk_coeff_token')       # [6][16][4]
C0 = parse_table(HEADER, 'vk_coeff0_token')      # [6]
TZ = parse_table(HEADER, 'vk_total_zeros')       # [15][16]
TZ2 = parse_table(HEADER, 'vk_total_zeros_2x2_dc')  # [3][4]
RBI = parse_table(HEADER, 'vk_run_before_init')  # [7][16]

if __name__ == '__main__':
    print('coeff_token[0][0] =', CT[0][0])
    print('coeff0_token =', C0)
    print('total_zeros[0][:4] =', TZ[0][:4])
