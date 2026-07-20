"""
User-mode runner for nX m88k binaries.

    python run88.py <binary> [args...]  [--trace N] [--textbase 0xN]

Loads the a.out image into flat memory, builds a BSD-style initial stack
(argc, argv[], NULL, envp[], NULL), and executes.  Traps are reported with full
register state so the syscall convention can be read off the first one rather
than assumed.
"""

import sys
import struct

sys.path.insert(0, __file__.rsplit("\\", 1)[0].rsplit("/", 1)[0])
from nxaout import AOut          # noqa: E402
from cpu import CPU, Memory, Trap, PAGE  # noqa: E402

STACK_TOP = 0x7FFF0000
HDR_PAGE = 8192


def load(path, textbase=0):
    a = AOut(path)
    mem = Memory()
    text = a.data[HDR_PAGE:HDR_PAGE + a.text]
    mem.load(textbase, text)
    datavaddr = (textbase + a.text + PAGE - 1) & ~(PAGE - 1)
    data = a.data[HDR_PAGE + a.text: HDR_PAGE + a.text + a.data_sz]
    mem.load(datavaddr, data)
    bssvaddr = datavaddr + a.data_sz
    mem.load(bssvaddr, b"\0" * a.bss)
    return a, mem, datavaddr, bssvaddr


def build_stack(mem, argv, envp=()):
    """BSD initial stack: sp -> argc, argv pointers, NULL, envp, NULL."""
    sp = STACK_TOP
    ptrs = []
    for s in list(argv) + list(envp):
        b = s.encode() + b"\0"
        sp -= len(b)
        mem.write(sp, b)
        ptrs.append(sp)
    sp &= ~7
    nargv = len(argv)
    # layout downward: [argc][argv0..][NULL][envp..][NULL]
    words = [nargv] + ptrs[:nargv] + [0] + ptrs[nargv:] + [0]
    sp -= 4 * len(words)
    sp &= ~7
    for i, v in enumerate(words):
        mem.w32(sp + 4 * i, v)
    return sp


def describe_trap(cpu, t, mem):
    print("\n*** TRAP vector=%d at pc=%08x after %d instructions"
          % (t.vector, t.pc, cpu.count))
    print(cpu.dump())
    print("\n  plausible syscall registers:")
    for n in (13, 12, 10, 9, 2, 3, 4, 5):
        v = cpu.get(n)
        extra = ""
        if 0 < v < 0x7FFF0000:
            s = mem.cstr(v, 40)
            if s and all(32 <= c < 127 or c in (9, 10) for c in s):
                extra = "  -> %r" % s
        print("    r%-2d = %08x (%d)%s" % (n, v, v, extra))


# 4.3BSD syscall numbers.  nX passes the number in r9 and arguments in r2..r9,
# trapping through `tb0 0, r0, 128`; the result comes back in r2.
MASK_ERR = 0xFFFFFFFF

SYS = {0: "indir", 64: "getpagesize", 1: "exit", 2: "fork", 3: "read", 4: "write", 5: "open", 6: "close",
       17: "brk", 19: "lseek", 20: "getpid", 33: "access", 38: "stat",
       54: "ioctl", 62: "fstat", 73: "munmap", 108: "sigvec",
       109: "sigblock", 110: "sigsetmask", 116: "gettimeofday",
       120: "readv", 121: "writev", 24: "getuid", 47: "getgid",
       36: "sync", 39: "getppid", 49: "getlogin"}


class Kernel:
    def __init__(self, mem, verbose=False):
        self.mem = mem
        self.verbose = verbose
        self.brk = 0x00100000
        self.fds = {}
        self.nextfd = 3
        self.exited = None
        self.unknown = set()
        self.err = None   # errno, or None on success

    def syscall(self, cpu):
        self.err = None
        n = cpu.get(9)
        a = [cpu.get(i) for i in range(2, 10)]
        name = SYS.get(n, "sys%d" % n)
        ret = 0
        if name == "exit":
            self.exited = a[0]
        elif name == "write":
            data = self.mem.read(a[1], a[2])
            out = sys.stdout if a[0] == 1 else sys.stderr
            out.write(data.decode("latin1"))
            out.flush()
            ret = a[2]
        elif name == "read":
            f = self.fds.get(a[0])
            if f is None:
                ret = 0                  # EOF on stdin
            else:
                data = f.read(a[2])
                self.mem.write(a[1], data)
                ret = len(data)
        elif name == "open":
            path = self.mem.cstr(a[0]).decode("latin1")
            try:
                self.fds[self.nextfd] = open(path, "rb")
                ret, self.nextfd = self.nextfd, self.nextfd + 1
            except OSError:
                self.err = 2             # ENOENT
        elif name == "lseek":
            f = self.fds.get(a[0])
            ret = f.seek(a[1], a[3] if len(a) > 3 else 0) if f else 0
        elif name == "gettimeofday":
            import time as _t
            now = _t.time()
            self.mem.write(a[0], struct.pack(">II", int(now),
                                             int((now % 1) * 1e6)))
            if a[1]:                     # struct timezone {minuteswest, dsttime}
                self.mem.write(a[1], struct.pack(">ii", 0, 0))
            ret = 0
        elif name == "writev":
            total = 0
            out = sys.stdout if a[0] == 1 else sys.stderr
            for i in range(a[2]):
                base, ln = struct.unpack(">II", self.mem.read(a[1] + 8 * i, 8))
                out.write(self.mem.read(base, ln).decode("latin1"))
                total += ln
            out.flush()
            ret = total
        elif name == "getpagesize":
            ret = 4096
        elif name == "indir":            # syscall(number, args...)
            cpu.set(9, a[0])
            for i in range(1, 7):
                cpu.set(1 + i, cpu.get(2 + i))
            return self.syscall(cpu)
        elif name == "brk":
            self.brk = a[0] or self.brk
            ret = 0
        elif name in ("getpid", "getppid"):
            ret = 4242
        elif name in ("getuid", "getgid", "sync"):
            ret = 0
        elif name == "close":
            f = self.fds.pop(a[0], None)
            if f: f.close()
            ret = 0
        elif name in ("sigvec", "sigblock", "sigsetmask", "ioctl"):
            ret = 0                      # harmless stubs
        elif name in ("fstat", "stat"):
            self.mem.write(a[1] if name == "fstat" else a[1], b"\0" * 64)
            ret = 0
        else:
            self.unknown.add(n)
            ret = 0
        if self.verbose:
            print("    [syscall %-12s (%s) = %d]"
                  % (name, ", ".join("%#x" % x for x in a[:4]), ret),
                  file=sys.stderr)
        cpu.set(2, ret)


def main(argv):
    args = [x for x in argv[1:] if not x.startswith("--")]
    opts = dict()
    for x in argv[1:]:
        if x.startswith("--"):
            k, _, v = x[2:].partition("=")
            opts[k] = v
    path = args[0]
    textbase = int(opts.get("textbase", "0"), 0)

    a, mem, dv, bv = load(path, textbase)
    print("%s\n  text %d @ %08x   data %d @ %08x   bss %d @ %08x   entry %08x"
          % (path, a.text, textbase, a.data_sz, dv, a.bss, bv, a.entry))

    cpu = CPU(mem)
    cpu.pc = a.entry if a.entry else textbase
    cpu.set(31, build_stack(mem, args))
    if "trace" in opts:
        cpu.trace = sys.stdout
        limit = int(opts["trace"] or "40")
    else:
        limit = int(opts.get("limit", "200000"))

    kern = Kernel(mem, verbose="v" in opts)
    print("--- output " + "-" * 50)
    try:
        while cpu.count < limit:
            try:
                cpu.step()
            except Trap as t:
                if t.vector == 128:
                    kern.syscall(cpu)
                    if kern.exited is not None:
                        print("\n" + "-" * 61)
                        print("exited(%d) after %d instructions"
                              % (kern.exited, cpu.count))
                        if kern.unknown:
                            print("unimplemented syscalls: %s"
                                  % sorted(kern.unknown))
                        return
                    cpu.pc = t.pc + (4 if kern.err else 8)
                    if kern.err:
                        cpu.set(2, kern.err)
                else:
                    describe_trap(cpu, t, mem)
                    return
        print("\n(instruction limit %d reached)" % limit)
        print(cpu.dump())
    except Exception as e:
        print("\n!! %s: %s at pc=%08x after %d instructions"
              % (type(e).__name__, e, cpu.pc, cpu.count))
        print(cpu.dump())


if __name__ == "__main__":
    main(sys.argv)
