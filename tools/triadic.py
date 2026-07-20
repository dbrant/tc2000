"""
Infer the opcode 0x3D (triadic register) sub-encoding from evidence.

Strategy: histogram the 11-bit function field over real kernel text, then pin
meanings using positional anchors that only one instruction can plausibly fill.

  * The word that most often ENDS a function is the return -- `jmp r1`
    (or `jmp.n r1`).  Its function field is therefore the jump encoding, and
    the S2 field should be r1 nearly every time.
  * Functions are entered by `bsr`; the instruction at a call site's delay slot
    and the operand shape (D/S1/S2 usage) separate 2-operand jumps from
    3-operand ALU ops.
  * `or rD, r0, rS2` is the canonical register-move idiom, so a very common
    function field with S1==r0 is `or`.
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
print(a.summary())

TRI = [(i, w) for i, w in enumerate(words) if (w >> 26) == 0x3D]
print("triadic words: %d" % len(TRI))

hist = collections.Counter(((w >> 5) & 0x7FF) for _, w in TRI)
print("\n--- function-field histogram (bits 15-5), top 30 ---")
print("  %-8s %-8s %7s   %s" % ("func", "binary", "count", "operand shape"))
for func, n in hist.most_common(30):
    ex = [w for _, w in TRI if ((w >> 5) & 0x7FF) == func][:400]
    d0 = sum(1 for w in ex if ((w >> 21) & 0x1F) == 0)
    s1_0 = sum(1 for w in ex if ((w >> 16) & 0x1F) == 0)
    s2 = collections.Counter(w & 0x1F for w in ex)
    shape = "D=r0 %3d%%  S1=r0 %3d%%  topS2=%s" % (
        100 * d0 // len(ex), 100 * s1_0 // len(ex),
        ",".join("r%d(%d%%)" % (r, 100 * c // len(ex))
                 for r, c in s2.most_common(2)))
    print("  0x%03x   %011s %7d   %s" % (func, format(func, "011b"), n, shape))

# ---- anchor 1: last instruction of each function -------------------------
print("\n--- anchor: final word of each function ---")
tail = collections.Counter()
tail_s2 = collections.Counter()
for addr, name, size in a.functions():
    if size < 8:
        continue
    idx = (addr - base) // 4 + size // 4 - 1
    for k in (idx, idx - 1):     # last word, and the one before (delay slot)
        if 0 <= k < nw and (words[k] >> 26) == 0x3D:
            f = (words[k] >> 5) & 0x7FF
            tail[f] += 1
            if f == ((words[k] >> 5) & 0x7FF):
                tail_s2[(f, words[k] & 0x1F)] += 1
for f, n in tail.most_common(6):
    regs = [(r, c) for (ff, r), c in tail_s2.most_common(40) if ff == f][:3]
    print("  func 0x%03x  %5d occurrences at function end   S2: %s" %
          (f, n, ", ".join("r%d x%d" % (r, c) for r, c in regs)))

# ---- anchor 2: what sits in a bsr delay slot / after calls ---------------
print("\n--- anchor: function-field usage where S1 == r0 (move idiom) ---")
mv = collections.Counter()
for _, w in TRI:
    if ((w >> 16) & 0x1F) == 0:
        mv[(w >> 5) & 0x7FF] += 1
for f, n in mv.most_common(6):
    print("  func 0x%03x  %5d with S1=r0" % (f, n))

# ---- decompose: does bits15-11 mirror the immediate opcode? -------------
print("\n--- decomposition test: hi=bits15-11, var=bits10-8, lo=bits7-5 ---")
dec = collections.Counter()
for _, w in TRI:
    f = (w >> 5) & 0x7FF
    dec[((f >> 6) & 0x1F, (f >> 3) & 0x07, f & 0x07)] += 1
for (hi, var, lo), n in dec.most_common(20):
    guess = m88k.ALU_IMM.get(hi) or m88k.LOAD_STORE.get(hi) or "?"
    print("  hi=0x%02x var=%d lo=%d  %7d   imm-table says: %s" %
          (hi, var, lo, n, guess))
