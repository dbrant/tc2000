"""
Symbol-annotated disassembler for nX / MC88100 binaries.

    python disas.py <binary> <symbol|0xaddr> [more...]
    python disas.py <binary> --list <pattern>

Delay slots are marked, since every .n-suffixed branch on the 88100 executes
the following instruction before transferring control -- misreading those is
the classic way to get 88k disassembly subtly wrong.
"""

import sys
import struct

sys.path.insert(0, __file__.rsplit("\\", 1)[0].rsplit("/", 1)[0])
from nxaout import AOut          # noqa: E402
import m88k                      # noqa: E402

DELAYED = {"br.n", "bsr.n", "bb0.n", "bb1.n", "bcnd.n", "jmp.n", "jsr.n"}


def disassemble(a, start, size, out=sys.stdout):
    base = a.text_base
    end = start + size
    prev_delayed = False
    for addr in range(start, end, 4):
        w = a.word_at(addr, base)
        if w is None:
            break
        ins = m88k.decode(w, addr)
        label = a.exact_sym(addr)
        if label and addr != start:
            out.write("\n%08x <%s>:\n" % (addr, label))
        mark = " |" if prev_delayed else "  "
        if ins is None:
            out.write("  %08x  %08x %s .word    0x%08x\n" % (addr, w, mark, w))
            prev_delayed = False
            continue
        out.write("  %08x  %08x %s %s\n"
                  % (addr, w, mark, ins.text(a.sym_for)))
        prev_delayed = ins.mn in DELAYED


def main(argv):
    path = argv[1]
    a = AOut(path)
    if argv[2] == "--list":
        pat = argv[3].lower()
        for addr, name, size in a.functions():
            if pat in name.lower():
                print("%08x  %6d  %s" % (addr, size, name))
        return
    print(a.summary())
    for want in argv[2:]:
        if want.startswith("0x"):
            addr, size, name = int(want, 16), 0x100, want
        else:
            hit = [(x, n, s) for x, n, s in a.functions() if n == want]
            if not hit:
                print("\n!! no symbol %r" % want)
                continue
            addr, name, size = hit[0]
        print("\n%s\n%08x <%s>:   (%d bytes)" % ("=" * 68, addr, name, size))
        disassemble(a, addr, size)


if __name__ == "__main__":
    main(sys.argv)
