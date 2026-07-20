"""
Find which lock the kernel boot deadlocks on.

_caller() (an alias for simple_lock) takes the lock address in r2 and is
reached via bsr, so r1 on entry names the caller.  Record both, resolve them
against the symbol table -- data/bss symbols as well as text, since locks live
in .data or .bss -- and report every lock the boot touches, flagging the one it
is still spinning on at the end.
"""

import sys
import bisect
import collections

sys.path.insert(0, __file__.rsplit("\\", 1)[0].rsplit("/", 1)[0])
from nxaout import AOut          # noqa: E402
from cpu import CPU              # noqa: E402
import sysmode as SM             # noqa: E402

LOCK_FN = 0xC0018170             # _caller / simple_lock
LOCK_LOOP = (0xC00181A4, 0xC00181AC)

a = AOut(SM.KERNEL)

# symbol lookup across *all* segments, keeping aliases
allsyms = collections.defaultdict(list)
for s in a.symbols:
    allsyms[s.value].append(s.name)
addrs = sorted(allsyms)


def sym(addr):
    i = bisect.bisect_right(addrs, addr) - 1
    if i < 0:
        return "?"
    base = addrs[i]
    names = "|".join(sorted(allsyms[base]))
    return names if base == addr else "%s+0x%x" % (names, addr - base)


mem = SM.WatchedMemory()
mem.load(SM.TEXT_BASE, a.data[8192:8192 + a.text])
mem.load(SM.DATA_BASE, a.data[8192 + a.text:8192 + a.text + a.data_sz])
mem.load(SM.DATA_BASE + a.data_sz, b"\0" * a.bss)
mem.map_region(SM.TEXT_BASE, a.text, "text")
mem.map_region(SM.DATA_BASE, a.data_sz + a.bss, "data+bss")

cpu = CPU(mem)
cpu.pc = a.entry
cpu.set(31, SM.DATA_BASE + a.data_sz + a.bss + 0x8000)

acquires = []                     # (count, lockaddr, retaddr)
order = collections.OrderedDict()
N = int(sys.argv[1]) if len(sys.argv) > 1 else 6000000
try:
    for i in range(N):
        mem.pc = cpu.pc
        mem.ticks = cpu.count * SM.TICK_SCALE
        if cpu.pc == LOCK_FN:
            lock, ret = cpu.get(2), cpu.get(1)
            acquires.append((cpu.count, lock, ret))
            order.setdefault((lock, ret), cpu.count)
        cpu.step()
except Exception as e:
    print("stopped:", type(e).__name__, e)

print("simple_lock() entered %d times; %d distinct (lock, caller) pairs\n"
      % (len(acquires), len(order)))

print("--- every distinct lock acquisition, in order ---")
for (lock, ret), when in order.items():
    print("  @%-9d lock %08x %-34s from %s"
          % (when, lock, sym(lock), sym(ret - 8)))

if acquires:
    last_count, last_lock, last_ret = acquires[-1]
    print("\n--- final (stuck) acquisition ---")
    print("  entered at instruction %d" % last_count)
    print("  lock   %08x  %s" % (last_lock, sym(last_lock)))
    print("  caller %08x  %s" % (last_ret - 8, sym(last_ret - 8)))
    print("  lock word now = %08x" % mem.r32(last_lock))
    print("  (nonzero means held; boot is spinning waiting for a release)")

print("\nfinal pc=%08x  %s   after %d instructions"
      % (cpu.pc, sym(cpu.pc), cpu.count))
