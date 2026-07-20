"""
Recover nX's system-call table by reading `sysent[]` out of the kernel image.

4.3BSD's dispatch table is an array of  struct sysent { int sy_narg;
int (*sy_call)(); }  -- 8 bytes per entry.  Resolving each sy_call through the
kernel symbol table names every syscall, including the nX-specific ones above
the 4.3BSD range that we hit while running tape binaries under emulation.

The kernel links data well above text (text at 0xC0010000, data at 0xC1000000),
so the data base is derived from the symbol table rather than assumed.
"""

import sys
import struct
import bisect

sys.path.insert(0, __file__.rsplit("\\", 1)[0].rsplit("/", 1)[0])
from nxaout import AOut          # noqa: E402

a = AOut(r"e:\git\tc2000\tapeimage\vmunix")
print(a.summary())

# ---- locate the data segment ------------------------------------------
data_syms = [s for s in a.symbols if (s.ntype & 0x1E) == 0x06]
dmin = min(s.value for s in data_syms)
DATA_BASE = dmin & ~0xFFFFF          # round down to a megabyte
DATA_FOFF = 8192 + a.text
print("data symbols: %d   lowest %08x   assumed data base %08x"
      % (len(data_syms), dmin, DATA_BASE))


def dread(vaddr, n):
    off = DATA_FOFF + (vaddr - DATA_BASE)
    if off < 0 or off + n > len(a.data):
        return None
    return a.data[off:off + n]


# sanity: a known data symbol should sit inside the data segment
probe = a.by_name.get("_putchar")
if probe:
    print("probe _putchar @ %08x -> file off %d"
          % (probe.value, DATA_FOFF + (probe.value - DATA_BASE)))

# ---- find the table ----------------------------------------------------
cand = [n for n in ("_sysent", "sysent", "_sysent0") if n in a.by_name]
print("sysent candidates:", cand)
if not cand:
    print("!! no sysent symbol; data symbols starting with _sys:")
    for s in sorted(data_syms, key=lambda s: s.value):
        if s.name.startswith("_sys"):
            print("   %08x %s" % (s.value, s.name))
    sys.exit(1)

sysent = a.by_name[cand[0]]
print("sysent @ %08x" % sysent.value)

# text symbols for reverse lookup
txt = sorted((s.value, s.name) for s in a.symbols if s.is_text)
taddr = [v for v, _ in txt]


def fname(addr):
    i = bisect.bisect_right(taddr, addr) - 1
    if i < 0:
        return None
    v, n = txt[i]
    return n if v == addr else "%s+0x%x" % (n, addr - v)


print("\n%4s %5s  %-28s %s" % ("num", "nargs", "handler", "addr"))
INTEREST = {0, 24, 36, 187, 188, 189, 190, 191}
rows = []
for i in range(0, 256):
    raw = dread(sysent.value + i * 8, 8)
    if raw is None:
        break
    narg, call = struct.unpack(">Ii", raw)
    call &= 0xFFFFFFFF
    if not (0xC0000000 <= call < 0xC0100000) or narg > 16:
        rows.append((i, narg, None, call))
        continue
    rows.append((i, narg, fname(call), call))

last = max((i for i, n, f, c in rows if f), default=0)
print("table appears to hold %d entries" % (last + 1))
for i, narg, f, c in rows[:last + 1]:
    mark = "  <==" if i in INTEREST else ""
    print("%4d %5d  %-28s %08x%s" % (i, narg, f or "-", c, mark))
