"""
Call kernel functions directly under emulation to recover their behaviour.

The paddr2* family is pure bit manipulation, so rather than deduce what `mak`
and `extu` do from their operand encodings, just run the real code with chosen
inputs and read the answers off.  A sentinel return address in r1 tells us when
the function has finished.

    python callfn.py
"""

import sys
import collections
import bisect

sys.path.insert(0, __file__.rsplit("\\", 1)[0].rsplit("/", 1)[0])
from nxaout import AOut          # noqa: E402
from cpu import CPU              # noqa: E402
import sysmode as SM             # noqa: E402

SENTINEL = 0xDEAD0000
STACK = 0xC1200000

a = AOut(SM.KERNEL)
allsyms = collections.defaultdict(list)
for s in a.symbols:
    allsyms[s.value].append(s.name)
addrs = sorted(allsyms)


def sym(x):
    i = bisect.bisect_right(addrs, x) - 1
    if i < 0:
        return "?"
    b = addrs[i]
    n = "|".join(sorted(allsyms[b]))
    return n if b == x else "%s+0x%x" % (n, x - b)


def fresh_mem():
    mem = SM.WatchedMemory()
    mem.load(SM.TEXT_BASE, a.data[8192:8192 + a.text])
    mem.load(SM.DATA_BASE, a.data[8192 + a.text:8192 + a.text + a.data_sz])
    mem.load(SM.DATA_BASE + a.data_sz, b"\0" * a.bss)
    mem.map_region(SM.TEXT_BASE, a.text, "t")
    mem.map_region(SM.DATA_BASE, a.data_sz + a.bss, "d")
    return mem


MEM = fresh_mem()


def call(name, args, budget=200000):
    """Call a kernel function; return (result, instructions, error)."""
    s = a.by_name.get(name)
    if not s:
        return None, 0, "no symbol"
    cpu = CPU(MEM)
    cpu.pc = s.value
    cpu.set(31, STACK)
    cpu.set(1, SENTINEL)
    for i, v in enumerate(args):
        cpu.set(2 + i, v)
    for _ in range(budget):
        if cpu.pc == SENTINEL:
            return cpu.get(2), cpu.count, None
        try:
            MEM.pc = cpu.pc
            MEM.ticks = cpu.count * SM.TICK_SCALE
            cpu.step()
        except Exception as e:
            return None, cpu.count, "%s at %08x (%s)" % (
                type(e).__name__, cpu.pc, sym(cpu.pc))
    return None, budget, "budget exceeded (last pc %08x %s)" % (cpu.pc, sym(cpu.pc))


def sweep(name, nargs=2):
    print("\n=== %s ===" % name)
    s = a.by_name.get(name)
    if not s:
        print("   (no symbol)")
        return
    # single walking bit through the first argument
    print("   walking a single bit through arg0 (arg1 = 0):")
    prev = None
    for bit in range(31, -1, -1):
        v = 1 << bit
        r, n, err = call(name, [v] + [0] * (nargs - 1))
        if err:
            print("     bit %2d: ERROR %s" % (bit, err))
            break
        if r != prev:
            print("     bit %2d  in=%08x -> %08x" % (bit, v, r))
        prev = r


for fn in ("_paddr2intrlvpnode", "_paddr2modulusramaddr", "_paddr2bank",
           "_paddr2memoffset", "_paddr2tintrlvramaddr", "_paddr2bypass",
           "_paddr2modulusdata"):
    sweep(fn)
