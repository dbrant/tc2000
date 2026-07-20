"""
MC88100 execution core.

Sequencing model: the 88100 has *explicit* delay slots.  A branch with the .n
suffix executes the following instruction before transferring control; a branch
without it does not.  Getting this backwards is the classic way to produce an
emulator that almost works, so the two cases are handled separately and
visibly.

Immediate logical ops follow the 88k convention where `and`/`or`/`xor` touch
only one halfword and leave the other intact, while `mask` zeroes it.  That is
what makes the compiler's `and.u rX,rX,0x7f` + `and rX,rX,0x8000` pair compose
into a single 0x007F8000 mask.
"""

import struct
import m88k

PAGE = 4096
MASK32 = 0xFFFFFFFF

# cmp deposits a condition bit-vector; these are the bit positions.
CMP_EQ, CMP_NE = 2, 3
CMP_GT, CMP_LE, CMP_LT, CMP_GE = 4, 5, 6, 7
CMP_HI, CMP_LS, CMP_LO, CMP_HS = 8, 9, 10, 11


def s32(v):
    v &= MASK32
    return v - (1 << 32) if v & 0x80000000 else v


class Memory:
    """Sparse paged memory."""

    def __init__(self):
        self.pages = {}

    def _page(self, addr):
        p = self.pages.get(addr >> 12)
        if p is None:
            p = self.pages[addr >> 12] = bytearray(PAGE)
        return p

    def read(self, addr, n):
        out = bytearray()
        while n:
            off = addr & (PAGE - 1)
            take = min(n, PAGE - off)
            out += self._page(addr)[off:off + take]
            addr += take
            n -= take
        return bytes(out)

    def write(self, addr, data):
        i = 0
        while i < len(data):
            off = addr & (PAGE - 1)
            take = min(len(data) - i, PAGE - off)
            self._page(addr)[off:off + take] = data[i:i + take]
            addr += take
            i += take

    def load(self, addr, data):
        self.write(addr, data)

    def r32(self, a):
        return struct.unpack(">I", self.read(a, 4))[0]

    def w32(self, a, v):
        self.write(a, struct.pack(">I", v & MASK32))

    def cstr(self, a, limit=4096):
        out = bytearray()
        while len(out) < limit:
            b = self.read(a, 1)[0]
            if b == 0:
                break
            out.append(b)
            a += 1
        return bytes(out)


class Trap(Exception):
    def __init__(self, vector, pc):
        super().__init__("trap %d at %08x" % (vector, pc))
        self.vector, self.pc = vector, pc


class CPU:
    def __init__(self, mem):
        self.mem = mem
        self.r = [0] * 32
        self.cr = [0] * 64
        self.pc = 0
        self.pending = None      # delay-slot branch target
        self.count = 0
        self.trace = None        # set to a file object to trace

    # -- register file: r0 is hardwired to zero ---------------------------
    def get(self, n):
        return 0 if n == 0 else self.r[n]

    def set(self, n, v):
        if n:
            self.r[n] = v & MASK32

    # -- main loop ---------------------------------------------------------
    def step(self):
        pc = self.pc
        w = self.mem.r32(pc)
        ins = m88k.decode(w, pc)
        if ins is None:
            raise Trap(-1, pc)
        if self.trace:
            self.trace.write("%08x  %08x  %s\n" % (pc, w, ins.text()))

        self.next_pc = pc + 4
        self.branch_to = None
        self.execute(ins, w)
        self.count += 1

        # sequencing -- delay slots are explicit
        if self.pending is not None:
            target, self.pending = self.pending, None
            self.pc = target
        elif self.branch_to is not None:
            if ins.mn.endswith(".n"):
                self.pending = self.branch_to
                self.pc = self.next_pc
            else:
                self.pc = self.branch_to
        else:
            self.pc = self.next_pc

    def run(self, limit=10_000_000):
        for _ in range(limit):
            self.step()
        raise RuntimeError("instruction limit reached")

    # -- execution ---------------------------------------------------------
    def execute(self, ins, w):
        op = (w >> 26) & 0x3F
        D = (w >> 21) & 0x1F
        S1 = (w >> 16) & 0x1F
        imm = w & 0xFFFF
        mn = ins.mn
        a = self.get(S1)

        # ---- memory, immediate and register forms ----
        if mn in MEM_OPS:
            if op < 0x10:                      # immediate displacement
                ea = (a + imm) & MASK32
            else:                              # triadic register form
                var = (w >> 5) & 0x1F
                idx = self.get(w & 0x1F)
                if var & 0x10:                 # scaled
                    idx = (idx * MEM_OPS[mn][0]) & MASK32
                ea = (a + idx) & MASK32
            return self.memop(mn, D, ea)

        # ---- logical / arithmetic ----
        if op < 0x20:                          # immediate form
            b = imm
        else:                                  # triadic register form
            b = self.get(w & 0x1F)

        if mn == "or":
            self.set(D, a | b)
        elif mn == "or.u":
            self.set(D, a | (b << 16))
        elif mn == "or.c":
            self.set(D, a | (~b & MASK32))
        elif mn == "and":
            # immediate `and` masks only the low halfword
            self.set(D, a & (0xFFFF0000 | b) if op < 0x20 else a & b)
        elif mn == "and.u":
            self.set(D, a & (0x0000FFFF | (b << 16)))
        elif mn == "and.c":
            self.set(D, a & (~b & MASK32))
        elif mn == "xor":
            self.set(D, a ^ b)
        elif mn == "xor.u":
            self.set(D, a ^ (b << 16))
        elif mn == "xor.c":
            self.set(D, a ^ (~b & MASK32))
        elif mn == "mask":
            self.set(D, a & b)
        elif mn == "mask.u":
            self.set(D, a & (b << 16))
        elif mn == "addu":
            self.set(D, a + b)
        elif mn == "subu":
            self.set(D, a - b)
        elif mn == "add":
            self.set(D, s32(a) + s32(b))
        elif mn == "sub":
            self.set(D, s32(a) - s32(b))
        elif mn == "mul":
            self.set(D, (a * b) & MASK32)
        elif mn == "divu":
            if b == 0:
                raise Trap(6, self.pc)
            self.set(D, a // b)
        elif mn == "div":
            if b == 0:
                raise Trap(6, self.pc)
            q = abs(s32(a)) // abs(s32(b))
            self.set(D, -q if (s32(a) < 0) != (s32(b) < 0) else q)
        elif mn == "cmp":
            self.set(D, self.compare(a, b))

        # ---- bit field ----
        elif mn in ("clr", "set", "ext", "extu", "mak", "rot"):
            if op == 0x3C:
                width, offset = (w >> 5) & 0x1F, w & 0x1F
            else:
                b2 = self.get(w & 0x1F)
                width, offset = (b2 >> 5) & 0x1F, b2 & 0x1F
            self.bitfield(mn, D, a, width, offset)

        # ---- control transfer ----
        elif mn in ("br", "br.n"):
            self.branch_to = ins.target
        elif mn in ("bsr", "bsr.n"):
            self.set(1, self.next_pc if mn == "bsr" else self.next_pc + 4)
            self.branch_to = ins.target
        elif mn in ("bb0", "bb0.n"):
            if not (a >> D) & 1:
                self.branch_to = ins.target
        elif mn in ("bb1", "bb1.n"):
            if (a >> D) & 1:
                self.branch_to = ins.target
        elif mn in ("bcnd", "bcnd.n"):
            if self.cond(D, a):
                self.branch_to = ins.target
        elif mn in ("jmp", "jmp.n"):
            self.branch_to = self.get(w & 0x1F) & ~3
        elif mn in ("jsr", "jsr.n"):
            tgt = self.get(w & 0x1F) & ~3
            self.set(1, self.next_pc if mn == "jsr" else self.next_pc + 4)
            self.branch_to = tgt

        # ---- traps ----
        elif mn == "tb0":
            if not (a >> D) & 1:
                raise Trap(w & 0x1FF, self.pc)
        elif mn == "tb1":
            if (a >> D) & 1:
                raise Trap(w & 0x1FF, self.pc)
        elif mn == "tcnd":
            if self.cond(D, a):
                raise Trap(w & 0x1FF, self.pc)

        # ---- control registers ----
        elif mn == "ldcr":
            self.set(D, self.cr[(w >> 5) & 0x3F])
        elif mn == "stcr":
            self.cr[(w >> 5) & 0x3F] = a
        elif mn == "xcr":
            c = (w >> 5) & 0x3F
            self.cr[c], old = a, self.cr[c]
            self.set(D, old)
        elif mn == "rte":
            raise Trap(-2, self.pc)
        elif mn.startswith(("f", "int", "nint", "trnc")):
            raise Trap(-3, self.pc)          # FPU not implemented yet
        else:
            raise Trap(-4, self.pc)

    def memop(self, mn, D, ea):
        m = self.mem
        if mn == "ld":
            self.set(D, m.r32(ea))
        elif mn == "ld.b":
            v = m.read(ea, 1)[0]
            self.set(D, v - 256 if v & 0x80 else v)
        elif mn == "ld.bu":
            self.set(D, m.read(ea, 1)[0])
        elif mn == "ld.h":
            v = struct.unpack(">H", m.read(ea, 2))[0]
            self.set(D, v - 65536 if v & 0x8000 else v)
        elif mn == "ld.hu":
            self.set(D, struct.unpack(">H", m.read(ea, 2))[0])
        elif mn == "ld.d":
            self.set(D, m.r32(ea))
            self.set(D + 1, m.r32(ea + 4))
        elif mn == "st":
            m.w32(ea, self.get(D))
        elif mn == "st.b":
            m.write(ea, bytes([self.get(D) & 0xFF]))
        elif mn == "st.h":
            m.write(ea, struct.pack(">H", self.get(D) & 0xFFFF))
        elif mn == "st.d":
            m.w32(ea, self.get(D))
            m.w32(ea + 4, self.get(D + 1))
        elif mn.startswith("lda"):
            self.set(D, ea)
        elif mn.startswith("xmem"):
            old = m.r32(ea) if mn == "xmem" else m.read(ea, 1)[0]
            if mn == "xmem":
                m.w32(ea, self.get(D))
            else:
                m.write(ea, bytes([self.get(D) & 0xFF]))
            self.set(D, old)

    def compare(self, a, b):
        sa, sb = s32(a), s32(b)
        v = 1 << 1
        v |= (1 << CMP_EQ) if a == b else (1 << CMP_NE)
        v |= (1 << CMP_GT) if sa > sb else 0
        v |= (1 << CMP_LE) if sa <= sb else 0
        v |= (1 << CMP_LT) if sa < sb else 0
        v |= (1 << CMP_GE) if sa >= sb else 0
        v |= (1 << CMP_HI) if a > b else 0
        v |= (1 << CMP_LS) if a <= b else 0
        v |= (1 << CMP_LO) if a < b else 0
        v |= (1 << CMP_HS) if a >= b else 0
        return v

    def cond(self, m5, a):
        sa = s32(a)
        return {0x01: sa > 0, 0x02: sa == 0, 0x03: sa >= 0,
                0x0C: sa < 0, 0x0D: sa != 0, 0x0E: sa <= 0}.get(m5, False)

    def bitfield(self, mn, D, a, width, offset):
        w = 32 if width == 0 else width
        m = (1 << w) - 1 if w < 32 else MASK32
        if mn == "extu":
            self.set(D, (a >> offset) & m)
        elif mn == "ext":
            v = (a >> offset) & m
            if w < 32 and v & (1 << (w - 1)):
                v -= (1 << w)
            self.set(D, v)
        elif mn == "mak":
            self.set(D, (a & m) << offset)
        elif mn == "clr":
            self.set(D, a & ~((m << offset) & MASK32))
        elif mn == "set":
            self.set(D, a | ((m << offset) & MASK32))
        elif mn == "rot":
            o = offset & 31
            self.set(D, ((a >> o) | (a << (32 - o))) & MASK32)

    def dump(self):
        out = []
        for i in range(0, 32, 4):
            out.append("  " + "  ".join("r%-2d=%08x" % (j, self.get(j))
                                        for j in range(i, i + 4)))
        return "pc=%08x  count=%d\n%s" % (self.pc, self.count, "\n".join(out))


# mnemonic -> (scale for indexed addressing,)
MEM_OPS = {
    "ld": (4,), "ld.b": (1,), "ld.bu": (1,), "ld.h": (2,), "ld.hu": (2,),
    "ld.d": (8,), "st": (4,), "st.b": (1,), "st.h": (2,), "st.d": (8,),
    "lda": (4,), "lda.b": (1,), "lda.h": (2,), "lda.d": (8,),
    "xmem": (4,), "xmem.bu": (1,),
}
