"""
`_sysent` lives in bss, so the dispatch table is populated at boot.  The
initialiser data must still be in .data, so find it structurally: scan the data
segment for the longest run of 8-byte records whose first word is a plausible
argument count (0..8) and whose second word is a text address.
"""

import sys
import struct
import bisect

sys.path.insert(0, __file__.rsplit("\\", 1)[0].rsplit("/", 1)[0])
from nxaout import AOut          # noqa: E402

a = AOut(r"e:\git\tc2000\tapeimage\vmunix")
DATA_BASE = 0xC1000000
DATA_FOFF = 8192 + a.text
seg = a.data[DATA_FOFF:DATA_FOFF + a.data_sz]
TEXT_LO, TEXT_HI = 0xC0010000, 0xC0010000 + a.text
print("data %08x..%08x (%d bytes);  text %08x..%08x"
      % (DATA_BASE, DATA_BASE + a.data_sz, a.data_sz, TEXT_LO, TEXT_HI))

txt = sorted((s.value, s.name) for s in a.symbols if s.is_text)
taddr = [v for v, _ in txt]


def fname(addr):
    i = bisect.bisect_right(taddr, addr) - 1
    if i < 0:
        return None
    v, n = txt[i]
    return n if v == addr else "%s+0x%x" % (n, addr - v)


def is_entry(off):
    if off + 8 > len(seg):
        return False
    narg, call = struct.unpack(">II", seg[off:off + 8])
    return narg <= 8 and TEXT_LO <= call < TEXT_HI


# find runs
runs = []
off = 0
while off + 8 <= len(seg):
    if is_entry(off):
        start = off
        while is_entry(off):
            off += 8
        if (off - start) // 8 >= 20:
            runs.append((start, (off - start) // 8))
    else:
        off += 4
runs.sort(key=lambda r: -r[1])
print("\ncandidate tables (>=20 entries):")
for start, n in runs[:6]:
    print("   data+%06x  vaddr %08x  %d entries" % (start, DATA_BASE + start, n))

if not runs:
    sys.exit("no candidate table found")

start, n = runs[0]
print("\n=== syscall table at %08x (%d entries) ===" % (DATA_BASE + start, n))
INTEREST = {0, 1, 3, 4, 5, 24, 36, 54, 108, 116, 121, 186, 187, 188, 189, 190}
for i in range(n):
    narg, call = struct.unpack(">II", seg[start + i * 8:start + i * 8 + 8])
    nm = fname(call) or "-"
    mark = "   <<<" if i in INTEREST else ""
    print("  %3d  nargs=%d  %s%s" % (i, narg, nm, mark))
