"""
Use BBN's frame-descriptor entries (n_type 0xC4) as an independent oracle.

Each entry binds a text address to either:
    ":MS<reg>,<dst-or-offset>,<size>;..."   registers saved / relocated
    ":MP<reg>,<size>;..."                   parameter registers
    "<name>.o"                              source module boundary

Two uses:
  1. MODULE MAP -- the ".o" entries give source-module address ranges, which is
     an independent statement of where each compilation unit lives.  That
     settles whether a symbol name is plausible for the code at its address.
  2. PROLOGUE ORACLE -- the ":MS" masks state exactly which registers a
     function saves.  Decoding the prologue and comparing register sets tests
     the load/store and immediate encodings against ground truth emitted by
     the original compiler.
"""

import sys
import re
import struct
import bisect
import collections

sys.path.insert(0, __file__.rsplit("\\", 1)[0].rsplit("/", 1)[0])
from nxaout import AOut          # noqa: E402
import m88k                      # noqa: E402

a = AOut(r"e:\git\tc2000\tapeimage\vmunix")
d = a.data
base = a.text_base
text = a.text_bytes()
nw = len(text) // 4
words = struct.unpack(">%dI" % nw, text[:nw * 4])

frames = []
for i in range(a.syms // 12):
    o = a.symoff + i * 12
    ntype, _oth, _desc, val, strx = struct.unpack(">BbhII", d[o:o + 12])
    if ntype == 0xC4:
        frames.append((val, a._string(a.stroff, strx)))

mods = sorted((v, s) for v, s in frames if s.endswith(".o"))
saves = [(v, s) for v, s in frames if s.startswith(":MS")]
print("frame descriptors: %d  (%d module marks, %d save masks)"
      % (len(frames), len(mods), len(saves)))

mod_addrs = [v for v, _ in mods]


def module_of(addr):
    i = bisect.bisect_right(mod_addrs, addr) - 1
    return mods[i][1] if i >= 0 else "?"


print("\n--- module map (first 12 of %d) ---" % len(mods))
for v, s in mods[:12]:
    print("   %08x  %s" % (v, s))

# ---------------------------------------------------------------- 1. names
print("\n" + "=" * 70)
print("MODULE CHECK on the disputed symbols")
for probe in (0xC0018138, 0xC0018170, 0xC009EFD0, 0xC009EFE8,
              0xC009A600, 0xC0099E2C, 0xC0016000, 0xC008A8C4):
    print("   %08x  symtab=%-26s module=%s"
          % (probe, a.exact_sym(probe) or "-", module_of(probe)))

print("\n   modules containing each disputed name:")
for want in ("_bzero", "_getblk", "_ttyoutput", "_simple_lock_failed",
             "_paddr2intrlvpnode", "_panic", "_splvme5"):
    s = a.by_name.get(want)
    if s:
        print("     %-22s %08x  %s" % (want, s.value, module_of(s.value)))

# ------------------------------------------------------------ 2. prologues
MS_RE = re.compile(r"(r\d+),(-?\d+),(\d+)")


def mask_regs(s):
    """Registers the mask says are saved to the frame (negative offsets)."""
    out = set()
    for reg, dst, _sz in MS_RE.findall(s):
        if dst.startswith("-"):
            out.add(int(reg[1:]))
    return out


def prologue_regs(addr, limit=24):
    """Registers stored to the frame in the first `limit` instructions."""
    out = set()
    idx = (addr - base) // 4
    for k in range(limit):
        j = idx + k
        if j < 0 or j >= nw:
            break
        ins = m88k.decode(words[j], addr + k * 4)
        if ins is None:
            continue
        # sub-word saves matter: a mask entry like "r3,-1,1" is a char
        # parameter spilled with st.b, not a word save
        if ins.mn in ("st", "st.d", "st.h", "st.b") and len(ins.ops) == 3 \
                and ins.ops[1] in ("r31", "r30"):
            r = int(ins.ops[0][1:])
            out.add(r)
            if ins.mn == "st.d":
                out.add(r + 1)
        if ins.mn in ("bsr", "bsr.n", "jmp", "jmp.n"):
            break
    return out


print("\n" + "=" * 70)
print("PROLOGUE ORACLE: compiler-emitted save mask vs decoded prologue")
exact = superset = mismatch = empty = 0
examples = []
for addr, s in saves:
    want = mask_regs(s)
    if not want:
        empty += 1
        continue
    got = prologue_regs(addr)
    if got == want:
        exact += 1
    elif want <= got:
        superset += 1
    else:
        mismatch += 1
        if len(examples) < 6:
            examples.append((addr, sorted(want), sorted(got), s[:60]))
tot = exact + superset + mismatch
print("  masks checked : %d  (%d had no frame saves)" % (tot, empty))
print("  exact match   : %d  (%.2f%%)" % (exact, 100.0 * exact / tot))
print("  decoded superset of mask : %d  (%.2f%%)"
      % (superset, 100.0 * superset / tot))
print("  MISMATCH      : %d  (%.2f%%)" % (mismatch, 100.0 * mismatch / tot))
for addr, want, got, s in examples:
    print("     %08x want=%s got=%s   %s" % (addr, want, got, s))
