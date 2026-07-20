"""
Dump nX's complete system-call table, recovered from the kernel image.

`_sysent` is in bss (populated at boot), but the initialiser is a const array
in .text at 0xC00488F4.  Each record is 8 bytes:

    uint32 sy_call     kernel handler
    uint16 sy_narg     argument count
    uint16 sy_flags

Syscall n is record n-1.  The base is anchored against calls proven by running
tape binaries under emulation: 4=write, 5=open, 121=writev.

Writes tools/nx-syscalls.txt.
"""

import sys
import struct
import collections

sys.path.insert(0, __file__.rsplit("\\", 1)[0].rsplit("/", 1)[0])
from nxaout import AOut          # noqa: E402

TEXT_LO = 0xC0010000
SYSENT = 0xC00488F4

a = AOut(r"e:\git\tc2000\tapeimage\vmunix")
buf = a.data[8192:8192 + a.text]

byaddr = collections.defaultdict(list)
for s in a.symbols:
    if s.is_text:
        byaddr[s.value].append(s.name)


def entry(n):
    off = (SYSENT - TEXT_LO) + (n - 1) * 8
    if off + 8 > len(buf):
        return None
    ptr, narg, flags = struct.unpack(">IHH", buf[off:off + 8])
    names = sorted(byaddr.get(ptr, []))
    return ptr, narg, flags, names


# `nosys` slots all share one handler; find it as the most common target
counts = collections.Counter()
for n in range(1, 230):
    e = entry(n)
    if e:
        counts[e[0]] += 1
nosys_ptr, nosys_n = counts.most_common(1)[0]
print("nosys handler %08x used by %d slots (aliases: %s)"
      % (nosys_ptr, nosys_n, ", ".join(byaddr.get(nosys_ptr, []))))

lines = []
implemented = 0
for n in range(1, 230):
    e = entry(n)
    if e is None:
        break
    ptr, narg, flags, names = e
    if ptr == nosys_ptr:
        lines.append("%4d  -      nosys" % n)
        continue
    implemented += 1
    nm = names[0].lstrip("_") if names else "<%08x>" % ptr
    alias = ("  [aka %s]" % ", ".join(names[1:])) if len(names) > 1 else ""
    lines.append("%4d  %d args %-24s %08x%s" % (n, narg, nm, ptr, alias))

out = "\n".join(lines)
open("nx-syscalls.txt", "w").write(out + "\n")
print("%d implemented syscalls of %d slots -> nx-syscalls.txt\n"
      % (implemented, len(lines)))

# where did the interesting ones actually land?
WANT = ("gettimeofday", "getuid", "getgid", "stat", "lstat", "fstat",
        "ioctl", "sigvec", "getdirentries", "sbrk", "obreak", "access",
        "cluster", "node", "icc")
print("locations of notable calls:")
for n in range(1, 230):
    e = entry(n)
    if not e or e[0] == nosys_ptr or not e[3]:
        continue
    nm = e[3][0].lstrip("_")
    if any(w in nm.lower() for w in WANT):
        print("   %4d  %d args  %s" % (n, e[1], nm))
