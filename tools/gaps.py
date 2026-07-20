"""
Characterise the instruction encodings still undecoded after the triadic solve:
primary opcodes 0x20 (control register), 0x21 (FPU) and the unknown 0x3C
bit-field subops.  Same approach as triadic.py -- histogram the sub-fields over
real kernel text and read the structure off the distribution.
"""

import sys
import struct
import collections

sys.path.insert(0, __file__.rsplit("\\", 1)[0].rsplit("/", 1)[0])
from nxaout import AOut          # noqa: E402
import m88k                      # noqa: E402

a = AOut(r"e:\git\tc2000\tapeimage\vmunix")
base = a.text_base
text = a.text_bytes()
nw = len(text) // 4
words = struct.unpack(">%dI" % nw, text[:nw * 4])


def ctx(i):
    """Nearest preceding symbol, for locating where an encoding is used."""
    return a.sym_for(base + i * 4)


for OP, title in ((0x20, "control register"), (0x21, "floating point")):
    sel = [(i, w) for i, w in enumerate(words) if (w >> 26) == OP]
    print("=" * 70)
    print("primary opcode 0x%02x  (%s):  %d words" % (OP, title, len(sel)))
    # try several plausible sub-field positions
    for name, shift, mask in (("bits15-11", 11, 0x1F),
                              ("bits15-10", 10, 0x3F),
                              ("bits15-8", 8, 0xFF),
                              ("bits10-5", 5, 0x3F)):
        h = collections.Counter((w >> shift) & mask for _, w in sel)
        print("  %-10s distinct=%-3d  top: %s" %
              (name, len(h),
               ", ".join("0x%02x x%d" % (v, c) for v, c in h.most_common(6))))
    print("  sample (addr, word, D, S1, low16, where):")
    for i, w in sel[:12]:
        print("     %08x %08x  D=r%-2d S1=r%-2d low16=%04x   %s"
              % (base + i * 4, w, (w >> 21) & 0x1F, (w >> 16) & 0x1F,
                 w & 0xFFFF, ctx(i)))
    print()

# --- unknown 0x3C subops ---
sel = [(i, w) for i, w in enumerate(words)
       if (w >> 26) == 0x3C and ((w >> 10) & 0x3F) not in m88k.BITFIELD]
print("=" * 70)
print("opcode 0x3C with unrecognised subop: %d words" % len(sel))
h = collections.Counter((w >> 10) & 0x3F for _, w in sel)
for sub, n in h.most_common(12):
    ex = [(i, w) for i, w in sel if ((w >> 10) & 0x3F) == sub][:4]
    print("  subop 0x%02x (%06s)  x%-5d" % (sub, format(sub, "06b"), n))
    for i, w in ex:
        print("      %08x %08x  D=r%-2d S1=r%-2d w=%-2d o=%-2d  %s"
              % (base + i * 4, w, (w >> 21) & 0x1F, (w >> 16) & 0x1F,
                 (w >> 5) & 0x1F, w & 0x1F, ctx(i)))

# --- leftover 0x3D ---
sel = [(i, w) for i, w in enumerate(words)
       if (w >> 26) == 0x3D and ((w >> 10) & 0x3F) not in m88k.TRIADIC]
print("\nopcode 0x3D with unrecognised subop: %d words" % len(sel))
h = collections.Counter((w >> 10) & 0x3F for _, w in sel)
for sub, n in h.most_common(8):
    i, w = [(i, w) for i, w in sel if ((w >> 10) & 0x3F) == sub][0]
    print("  subop 0x%02x x%-4d  e.g. %08x %08x  %s"
          % (sub, n, base + i * 4, w, ctx(i)))
