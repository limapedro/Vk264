#!/usr/bin/env python3
"""Generate a mobile-readable SVG progress sheet for the GitHub README.

Reads docs/FUNCTION_INVENTORY.md and renders every PORTED row plus a
Call-of-Duty-style weapon level / XP bar for x264 parity progress.
No external fonts, CSS or scripts — safe for GitHub rendering.
"""
import html, pathlib, re

R = pathlib.Path(__file__).resolve().parents[1]
INV = R / 'docs' / 'FUNCTION_INVENTORY.md'
OUT = R / 'docs' / 'phoenix-progress.svg'

text = INV.read_text(encoding='utf-8')
total = int(re.search(r'Total x264 C functions indexed: \*\*(\d+)\*\*', text).group(1))
ported = []
for line in text.splitlines():
    if line.startswith('|') and '**PORTED**' in line:
        parts = [p.strip() for p in line.strip().strip('|').split('|')]
        if len(parts) >= 3 and parts[0].startswith('`'):
            ported.append((parts[0].strip('`'), parts[1].strip('`')))
missing = total - len(ported)
pct = 100.0 * len(ported) / total

W = 480
ROW = 42
HEAD = 560
FOOT = 120
H = HEAD + ROW*len(ported) + FOOT

o = []
a = o.append
a(f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}" role="img" aria-label="Phoenix x264 parity progress">')
a('<defs>')
a('<linearGradient id="bg" x1="0" y1="0" x2="0" y2="1">')
a('<stop offset="0" stop-color="#101820"/><stop offset="1" stop-color="#0a0f14"/>')
a('</linearGradient>')
a('<linearGradient id="xp" x1="0" y1="0" x2="1" y2="0">')
a('<stop offset="0" stop-color="#00e676"/><stop offset="1" stop-color="#69f0ae"/>')
a('</linearGradient>')
a('</defs>')
a(f'<rect width="{W}" height="{H}" fill="url(#bg)"/>')

# Header — weapon-card style.
a('<text x="24" y="48" font-family="sans-serif" font-size="30" font-weight="700" fill="#ffffff">VK264 / PHOENIX</text>')
a('<text x="24" y="78" font-family="sans-serif" font-size="17" fill="#8aa0b4">X264 PARITY PROGRESS  •  MOBILE VIEW</text>')
a('<rect x="24" y="100" width="432" height="150" rx="14" fill="#16212b" stroke="#274050"/>')
a('<text x="44" y="144" font-family="sans-serif" font-size="16" fill="#8aa0b4">WEAPON</text>')
a('<text x="44" y="176" font-family="sans-serif" font-size="26" font-weight="700" fill="#ffffff">X264 ORACLE</text>')
a(f'<text x="440" y="176" text-anchor="end" font-family="sans-serif" font-size="34" font-weight="700" fill="#00e676">LVL {len(ported)}</text>')
a('<text x="44" y="206" font-family="sans-serif" font-size="15" fill="#8aa0b4">XP</text>')
a(f'<text x="440" y="206" text-anchor="end" font-family="sans-serif" font-size="17" fill="#ffffff">{len(ported)} / {total}  ({pct:.1f}%)</text>')
# XP bar with segment ticks.
a('<rect x="44" y="222" width="392" height="18" rx="8" fill="#22303c"/>')
barw = max(10, int(392 * len(ported) / total))
a(f'<rect x="44" y="222" width="{barw}" height="18" rx="8" fill="url(#xp)"/>')
for i in range(1, 10):
    x = 44 + int(392*i/10)
    a(f'<line x1="{x}" y1="222" x2="{x}" y2="240" stroke="#101820" stroke-width="2"/>')

# Stat chips.
stats = [('BYTE-EXACT', '0 / 100', '#ff5252'),
         ('UNIT TESTS', '5 / 5', '#00e676'),
         ('PRESETS', '10', '#40c4ff'),
         ('FIXTURES', '5', '#ffca28')]
cw = 102
for i, (label, val, col) in enumerate(stats):
    x = 24 + i*cw
    a(f'<rect x="{x}" y="272" width="{cw-8}" height="74" rx="10" fill="#16212b" stroke="#274050"/>')
    a(f'<text x="{x+8}" y="296" font-family="sans-serif" font-size="12" fill="#8aa0b4">{label}</text>')
    a(f'<text x="{x+8}" y="326" font-family="sans-serif" font-size="20" font-weight="700" fill="{col}">{val}</text>')

# Milestones — weapon levelling unlocks.
a('<text x="24" y="382" font-family="sans-serif" font-size="18" font-weight="700" fill="#ffffff">UNLOCK TRACK</text>')
miles = [('10', 'CACHE', len(ported) >= 10), ('20', 'BITSTREAM', len(ported) >= 20),
         ('50', 'CABAC', False), ('100', 'ENCODER', False), ('100%', 'PARITY', False)]
mx = 24
for lvl, name, done in miles:
    col = '#00e676' if done else '#3a4a58'
    a(f'<rect x="{mx}" y="398" width="84" height="58" rx="9" fill="#16212b" stroke="{col}" stroke-width="2"/>')
    a(f'<text x="{mx+42}" y="422" text-anchor="middle" font-family="sans-serif" font-size="18" font-weight="700" fill="{col}">{lvl}</text>')
    a(f'<text x="{mx+42}" y="444" text-anchor="middle" font-family="sans-serif" font-size="12" fill="#b0c4d4">{name}</text>')
    mx += 90

a('<text x="24" y="504" font-family="sans-serif" font-size="20" font-weight="700" fill="#ffffff">PORTED FUNCTIONS</text>')
a(f'<text x="456" y="504" text-anchor="end" font-family="sans-serif" font-size="15" fill="#00e676">{len(ported)} UNLOCKED</text>')
a(f'<text x="24" y="530" font-family="sans-serif" font-size="14" fill="#8aa0b4">{missing} functions still locked • 100% byte-exact is the final unlock</text>')

# One row per ported function — every ported function is shown.
y = HEAD
for i, (name, src) in enumerate(ported):
    if i % 2 == 0:
        a(f'<rect x="16" y="{y-28}" width="448" height="{ROW}" rx="6" fill="#121c25"/>')
    a(f'<circle cx="32" cy="{y-7}" r="5" fill="#00e676"/>')
    a(f'<text x="46" y="{y}" font-family="sans-serif" font-size="17" fill="#ffffff">{html.escape(name[:34])}</text>')
    a(f'<text x="46" y="{y+16}" font-family="sans-serif" font-size="12" fill="#7f97aa">{html.escape(src[:42])}</text>')
    y += ROW

a(f'<rect x="0" y="{H-FOOT}" width="{W}" height="{FOOT}" fill="#0a0f14"/>')
a(f'<text x="24" y="{H-FOOT+34}" font-family="sans-serif" font-size="14" fill="#8aa0b4">Source: docs/FUNCTION_INVENTORY.md • regenerated by</text>')
a(f'<text x="24" y="{H-FOOT+58}" font-family="sans-serif" font-size="14" fill="#00e676">docs/generate_progress_svg.py</text>')
a('</svg>')

OUT.write_text('\n'.join(o) + '\n', encoding='utf-8')
print(f'wrote {OUT} ({H}px tall, {len(ported)} ported rows, {pct:.1f}%)')
