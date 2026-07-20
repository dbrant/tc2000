"""
Empirical validation of the MC88100 decoder.

The opcode table in m88k.py is reconstructed, so it needs evidence, not faith.
Three independent oracles, in increasing order of how much they prove:

  1. COVERAGE  -- fraction of text words that decode at all.  Real compiled
                  code should be near-total.  A bad table shows up as holes.

  2. CALL TARGETS -- every bsr/bsr.n target should land exactly on a function
                  symbol.  This tests opcode identification, the signed 26-bit
                  offset, the <<2 scaling and the PC base all at once.  Random
                  noise hits an exact symbol essentially never, so a high rate
                  here is very strong evidence.

  3. PROLOGUES -- functions should begin by adjusting r31 (stack pointer) and
                  saving the link register / callee-saved registers.  Tests the
                  immediate load/store and arithmetic encodings.

Usage:  python validate.py <binary>
"""

import sys
import struct
import collections

sys.path.insert(0, __file__.rsplit("\\", 1)[0].rsplit("/", 1)[0])
from nxaout import AOut          # noqa: E402
import m88k                      # noqa: E402


def run(path):
    a = AOut(path)
    print(a.summary())
    base = a.text_base
    print("text base (from symbols): %08x   entry: %08x" % (base, a.entry))

    text = a.text_bytes()
    nwords = len(text) // 4
    words = struct.unpack(">%dI" % nwords, text[:nwords * 4])

    # ---------------------------------------------------------- 1. coverage
    decoded = [None] * nwords
    unknown_ops = collections.Counter()
    nknown = 0
    for i, w in enumerate(words):
        ins = m88k.decode(w, base + i * 4)
        decoded[i] = ins
        if ins is not None:
            nknown += 1
        else:
            unknown_ops[(w >> 26) & 0x3F] += 1

    print("\n--- 1. coverage ---")
    print("  %d/%d words decode  (%.2f%%)" %
          (nknown, nwords, 100.0 * nknown / nwords))
    if unknown_ops:
        print("  top undecoded primary opcodes:")
        for op, n in unknown_ops.most_common(10):
            print("     op 0x%02x : %6d  (%.2f%%)" %
                  (op, n, 100.0 * n / nwords))

    # ------------------------------------------------------ 2. call targets
    print("\n--- 2. call targets ---")
    hits = miss = 0
    inrange = 0
    bad_examples = []
    for ins in decoded:
        if ins is None or ins.mn not in ("bsr", "bsr.n"):
            continue
        t = ins.target
        if not (base <= t < base + a.text):
            continue
        inrange += 1
        if a.exact_sym(t):
            hits += 1
        else:
            miss += 1
            if len(bad_examples) < 5:
                bad_examples.append((ins.addr, t, a.sym_for(t)))
    if inrange:
        print("  %d bsr targets in text; %d land exactly on a function symbol "
              "(%.2f%%)" % (inrange, hits, 100.0 * hits / inrange))
        for addr, t, near in bad_examples:
            print("     miss: %08x -> %08x  (nearest %s)" % (addr, t, near))
    else:
        print("  no bsr instructions decoded -- opcode table likely wrong")

    # also check bb0/bb1/bcnd stay inside the function they came from
    intra = out = 0
    for ins in decoded:
        if ins is None or ins.mn not in ("bb0", "bb0.n", "bb1", "bb1.n",
                                         "bcnd", "bcnd.n", "br", "br.n"):
            continue
        if ins.target is None:
            continue
        if base <= ins.target < base + a.text:
            intra += 1
        else:
            out += 1
    print("  conditional/uncond branches: %d in text, %d out of range" %
          (intra, out))

    # --------------------------------------------------------- 3. prologues
    print("\n--- 3. function prologues ---")
    funcs = [f for f in a.functions() if f[2] >= 16]
    pat = collections.Counter()
    good = 0
    for addr, name, _size in funcs:
        idx = (addr - base) // 4
        if idx < 0 or idx + 4 > nwords:
            continue
        first = [decoded[idx + k] for k in range(4)]
        mns = tuple(i.mn if i else "?" for i in first)
        pat[mns[0]] += 1
        # a real prologue adjusts the stack pointer or saves the link register
        for ins in first:
            if ins is None:
                continue
            if ins.mn == "subu" and ins.ops[:2] == ["r31", "r31"]:
                good += 1
                break
            if ins.mn in ("st", "st.d") and ins.ops[1] == "r31":
                good += 1
                break
    print("  %d functions examined; %d begin with a recognisable prologue "
          "(%.2f%%)" % (len(funcs), good,
                        100.0 * good / len(funcs) if funcs else 0))
    print("  most common first instruction:")
    for mn, n in pat.most_common(8):
        print("     %-10s %5d" % (mn, n))

    return a, decoded, base


if __name__ == "__main__":
    run(sys.argv[1] if len(sys.argv) > 1 else
        r"e:\git\tc2000\tapeimage\vmunix")
