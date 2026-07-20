"""
System-mode bring-up: execute the nX kernel itself.

This is deliberately a *reconnaissance* harness rather than a finished machine.
Nothing is known yet about the TC2000's device register map, so instead of
guessing, memory accesses that fall outside the loaded kernel image are logged
by region.  Whatever the kernel touches first IS the hardware that has to exist
next -- the boot path tells us the device map rather than the other way round.

    python sysmode.py [--limit N] [--trace N]
"""

import sys
import struct
import collections

sys.path.insert(0, __file__.rsplit("\\", 1)[0].rsplit("/", 1)[0])
from nxaout import AOut          # noqa: E402
from cpu import CPU, Memory, Trap  # noqa: E402
import m88k                      # noqa: E402

KERNEL = r"e:\git\tc2000\tapeimage\vmunix"
TEXT_BASE = 0xC0010000
DATA_BASE = 0xC1000000
HDR = 8192


class WatchedMemory(Memory):
    """Memory that records accesses outside the regions we deliberately map."""

    def __init__(self):
        super().__init__()
        self.mapped = []            # (lo, hi, name)
        self.foreign = collections.Counter()
        self.first_touch = {}
        self.console = []
        self.pc = 0

    def map_region(self, lo, size, name):
        self.mapped.append((lo, lo + size, name))

    def _check(self, addr):
        for lo, hi, _ in self.mapped:
            if lo <= addr < hi:
                return
        region = addr & 0xFFFF0000
        self.foreign[region] += 1
        self.first_touch.setdefault(region, (self.pc, addr))

    def read(self, addr, n):
        self._check(addr)
        return super().read(addr, n)

    def write(self, addr, data):
        self._check(addr)
        # candidate console/DUART regions: record byte writes so any boot
        # message the kernel emits becomes visible
        if 0xE0780000 <= addr < 0xE0790000 or 0xFE000000 <= addr < 0xFE010000:
            for b in data:
                if 32 <= b < 127 or b in (10, 13):
                    self.console.append((addr, b))
        return super().write(addr, data)


def main(argv):
    opts = {}
    for x in argv[1:]:
        if x.startswith("--"):
            k, _, v = x[2:].partition("=")
            opts[k] = v

    a = AOut(KERNEL)
    mem = WatchedMemory()
    mem.load(TEXT_BASE, a.data[HDR:HDR + a.text])
    mem.load(DATA_BASE, a.data[HDR + a.text:HDR + a.text + a.data_sz])
    mem.load(DATA_BASE + a.data_sz, b"\0" * a.bss)
    mem.map_region(TEXT_BASE, a.text, "text")
    mem.map_region(DATA_BASE, a.data_sz + a.bss, "data+bss")
    print("kernel loaded: text %08x+%x   data %08x+%x   bss +%x   entry %08x"
          % (TEXT_BASE, a.text, DATA_BASE, a.data_sz, a.bss, a.entry))

    cpu = CPU(mem)
    cpu.pc = a.entry
    cpu.set(31, DATA_BASE + a.data_sz + a.bss + 0x8000)   # provisional stack
    if "trace" in opts:
        cpu.trace = sys.stdout
    limit = int(opts.get("limit", "200000"))

    reason = "instruction limit"
    try:
        while cpu.count < limit:
            mem.pc = cpu.pc
            cpu.step()
    except Trap as t:
        reason = "trap vector %d" % t.vector
    except Exception as e:
        reason = "%s: %s" % (type(e).__name__, e)

    print("\nstopped: %s" % reason)
    print("  after %d instructions, pc=%08x  (%s)"
          % (cpu.count, cpu.pc, a.sym_for(cpu.pc)))
    w = mem.r32(cpu.pc) if True else 0
    ins = m88k.decode(w, cpu.pc)
    print("  faulting insn: %08x  %s" % (w, ins.text() if ins else "<bad>"))

    print("\n--- memory touched outside the kernel image ---")
    if not mem.foreign:
        print("  (none)")
    for region, n in mem.foreign.most_common(15):
        pc, addr = mem.first_touch[region]
        print("  %08x-%08x  %6d accesses   first %08x from %s"
              % (region, region + 0xFFFF, n, addr, a.sym_for(pc)))

    print("\n--- call path (nearest symbol to pc) ---")
    print("  %s" % a.sym_for(cpu.pc))


if __name__ == "__main__":
    main(sys.argv)
