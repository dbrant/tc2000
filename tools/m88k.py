"""
MC88100 instruction decoder.

Encoding is fixed-width 32-bit big-endian. The top 6 bits select the format:

    0x00-0x0F   load / store / exchange, 16-bit immediate
    0x10-0x1F   logical & arithmetic, 16-bit immediate
    0x20        control-register ops (ldcr/stcr/xcr and FPU-CR variants)
    0x21        floating point arithmetic
    0x30-0x37   branches (br/bsr, bb0/bb1)
    0x3A-0x3B   bcnd
    0x3C        bit-field, immediate width/offset
    0x3D        triadic register form
    0x3E-0x3F   (unassigned on 88100)

Confidence is tracked per-entry.  Anything not positively identified decodes to
None so the caller can emit `.word` rather than a plausible-looking lie -- with
a reconstructed table that distinction is the whole point.
"""

# ---------------------------------------------------------------- immediate --

# op -> (mnemonic, kind)
#   kind 'mem'  : rD, rS1, imm16      (unsigned displacement)
#   kind 'alu'  : rD, rS1, imm16
LOAD_STORE = {
    0x00: "xmem.bu", 0x01: "xmem",
    0x02: "ld.hu",   0x03: "ld.bu",
    0x04: "ld.d",    0x05: "ld",     0x06: "ld.h",   0x07: "ld.b",
    0x08: "st.d",    0x09: "st",     0x0A: "st.h",   0x0B: "st.b",
    0x0C: "lda.d",   0x0D: "lda",    0x0E: "lda.h",  0x0F: "lda.b",
}

ALU_IMM = {
    0x10: "and",  0x11: "and.u", 0x12: "mask", 0x13: "mask.u",
    0x14: "xor",  0x15: "xor.u", 0x16: "or",   0x17: "or.u",
    0x18: "addu", 0x19: "subu",  0x1A: "divu", 0x1B: "mul",
    0x1C: "add",  0x1D: "sub",   0x1E: "div",  0x1F: "cmp",
}

# ------------------------------------------------------------------ branches --

UNCOND = {0x30: "br", 0x31: "br.n", 0x32: "bsr", 0x33: "bsr.n"}
BITBR  = {0x34: "bb0", 0x35: "bb0.n", 0x36: "bb1", 0x37: "bb1.n"}
CONDBR = {0x3A: "bcnd", 0x3B: "bcnd.n"}

# bcnd condition field (bits 25-21).  Only the architecturally defined
# encodings; others are reserved.
BCND = {
    0x01: "gt0", 0x02: "eq0", 0x03: "ge0",
    0x0C: "lt0", 0x0D: "ne0", 0x0E: "le0",
}

# --------------------------------------------------------------- bit-field ---

# bits 15-10 of opcode 0x3C
BITFIELD = {
    0x20: "clr", 0x22: "set", 0x24: "ext",
    0x26: "extu", 0x28: "mak", 0x2A: "rot",
}

# The trap family shares opcode 0x3C but carries a 9-bit vector in bits 8-0
# instead of a width/offset pair.  Identified by context: subop 0x34 with
# vector 128 appears in _szicode and _pmap_bootstrap_vecpage -- the syscall
# path.  Since r0 reads as zero, `tb0 0, r0, vec` is an unconditional trap,
# which is the standard m88k system-call idiom.
TRAP = {0x34: "tb0", 0x36: "tb1", 0x3A: "tcnd"}

# Control-register ops, opcode 0x20, sub-opcode in bits 15-11.
# Anchored by `ldcr r10, cr4` (SXIP) inside _Xopcode and `stcr r1, cr18`
# across every _X* exception handler.
CTLREG = {
    0x08: ("ldcr", "load"), 0x09: ("fldcr", "load"),
    0x10: ("stcr", "store"), 0x11: ("fstcr", "store"),
    0x18: ("xcr", "exch"), 0x19: ("fxcr", "exch"),
}

# FPU, opcode 0x21, operation in bits 15-11.  The distribution over kernel
# text matches the standard m88k numbering exactly.
FPU = {
    0x00: "fmul", 0x04: "flt", 0x05: "fadd", 0x06: "fsub", 0x07: "fcmp",
    0x09: "int", 0x0A: "nint", 0x0B: "trnc", 0x0E: "fdiv", 0x0F: "fsqrt",
}
FPU_UNARY = {0x04, 0x09, 0x0A, 0x0B, 0x0F}
PREC = {0: "s", 1: "d", 2: "x"}

# ----------------------------------------------------------------- triadic ---

# Opcode 0x3D.  Determined empirically from kernel text (see triadic.py):
#
#     bits 15-10  opcode      -- same numbering as the immediate map
#     bits  9-5   variant     -- 0x10 selects scaled indexing on memory ops
#     bits  4-0   S2
#
# Anchors that pin this down: opcode 0x30 with S2=r1 is the final instruction
# of 2165 functions (the return, `jmp r1`); opcode 0x16 accounts for 22239
# words of which 89% have S1=r0 (the register-move idiom `or rD,r0,rS2`).
# The jump group at 0x30-0x33 mirrors br/br.n/bsr/bsr.n in the branch space,
# and the logical block reuses the immediate opcodes with .u replaced by .c.
TRIADIC = {
    0x00: "xmem.bu", 0x01: "xmem",
    0x02: "ld.hu",   0x03: "ld.bu",
    0x04: "ld.d",    0x05: "ld",     0x06: "ld.h",   0x07: "ld.b",
    0x08: "st.d",    0x09: "st",     0x0A: "st.h",   0x0B: "st.b",
    0x0C: "lda.d",   0x0D: "lda",    0x0E: "lda.h",  0x0F: "lda.b",
    0x10: "and",     0x11: "and.c",
    0x14: "xor",     0x15: "xor.c",
    0x16: "or",      0x17: "or.c",
    0x18: "addu",    0x19: "subu",   0x1A: "divu",   0x1B: "mul",
    0x1C: "add",     0x1D: "sub",    0x1E: "div",    0x1F: "cmp",
    0x20: "clr",     0x22: "set",    0x24: "ext",
    0x26: "extu",    0x28: "mak",    0x2A: "rot",
    0x30: "jmp",     0x31: "jmp.n",  0x32: "jsr",    0x33: "jsr.n",
    0x34: "ff1",     0x36: "ff0",
    0x3E: "tbnd",    0x3F: "rte",
}

# Opcodes taking only S2 (an indirect target), and those taking no operands.
TRIADIC_ONE_OP = {0x30, 0x31, 0x32, 0x33, 0x34, 0x36}
TRIADIC_NO_OP = {0x3F}
TRIADIC_MEM = set(range(0x00, 0x10))

VARIANT_SCALED = 0x10

CONTROL = {0x40: "ldcr", 0x80: "stcr", 0xC0: "xcr"}

REG_SP, REG_LINK = 31, 1


def _r(n):
    return "r%d" % n


def _signed(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


class Insn:
    __slots__ = ("addr", "word", "mn", "ops", "target", "conf")

    def __init__(self, addr, word, mn, ops, target=None, conf="ok"):
        self.addr, self.word = addr, word
        self.mn, self.ops = mn, ops
        self.target = target      # branch/call destination, if any
        self.conf = conf          # 'ok' | 'low'

    def text(self, symfn=None):
        s = "%-9s %s" % (self.mn, ", ".join(self.ops))
        if self.target is not None and symfn:
            nm = symfn(self.target)
            if nm:
                s += "   <%s>" % nm
        return s

    def __repr__(self):
        return "<%08x %s>" % (self.addr, self.text())


def decode(word, addr=0):
    """Decode one 32-bit big-endian instruction word.

    Returns an Insn, or None if the encoding is not positively identified.
    """
    op = (word >> 26) & 0x3F
    D = (word >> 21) & 0x1F
    S1 = (word >> 16) & 0x1F
    imm16 = word & 0xFFFF

    # --- load / store / exchange, immediate ---
    if op in LOAD_STORE:
        return Insn(addr, word, LOAD_STORE[op],
                    [_r(D), _r(S1), "0x%x" % imm16])

    # --- logical / arithmetic, immediate ---
    if op in ALU_IMM:
        return Insn(addr, word, ALU_IMM[op],
                    [_r(D), _r(S1), "0x%x" % imm16])

    # --- control register ---
    if op == 0x20:
        sub = (word >> 11) & 0x1F
        cr = (word >> 5) & 0x3F
        if sub not in CTLREG:
            return None
        mn, kind = CTLREG[sub]
        crname = "%scr%d" % ("f" if mn[0] == "f" else "", cr)
        if kind == "load":
            return Insn(addr, word, mn, [_r(D), crname])
        if kind == "store":
            return Insn(addr, word, mn, [_r(S1), crname])
        return Insn(addr, word, mn, [_r(D), _r(S1), crname])

    # --- floating point ---
    if op == 0x21:
        fop = (word >> 11) & 0x1F
        if fop not in FPU:
            return None
        # Operand-size fields.  TD is the LOW pair (bits 6-5) and T2 the high
        # one (10-9) -- not the other way round.  With them swapped this
        # printed `flt.sd` for what is really `flt.ds` (int->double) and
        # `fcmp.dds` for a compare of two doubles, which sent one debugging
        # session a long way down the wrong path.
        td = PREC.get((word >> 5) & 3, "?")
        t1 = PREC.get((word >> 7) & 3, "?")
        t2 = PREC.get((word >> 9) & 3, "?")
        S2 = word & 0x1F
        if fop in FPU_UNARY:
            return Insn(addr, word, "%s.%s%s" % (FPU[fop], td, t2),
                        [_r(D), _r(S2)])
        return Insn(addr, word, "%s.%s%s%s" % (FPU[fop], td, t1, t2),
                    [_r(D), _r(S1), _r(S2)])

    # --- unconditional branch / call: 26-bit signed word offset ---
    if op in UNCOND:
        off = _signed(word & 0x03FFFFFF, 26)
        tgt = (addr + (off << 2)) & 0xFFFFFFFF
        return Insn(addr, word, UNCOND[op], ["0x%08x" % tgt], target=tgt)

    # --- bit-test branch: bit number in D, 16-bit signed word offset ---
    if op in BITBR:
        off = _signed(imm16, 16)
        tgt = (addr + (off << 2)) & 0xFFFFFFFF
        return Insn(addr, word, BITBR[op],
                    ["%d" % D, _r(S1), "0x%08x" % tgt], target=tgt)

    # --- conditional branch ---
    if op in CONDBR:
        off = _signed(imm16, 16)
        tgt = (addr + (off << 2)) & 0xFFFFFFFF
        cond = BCND.get(D)
        if cond is None:
            return None
        return Insn(addr, word, CONDBR[op],
                    [cond, _r(S1), "0x%08x" % tgt], target=tgt)

    # --- bit-field with immediate width/offset, and the trap family ---
    if op == 0x3C:
        sub = (word >> 10) & 0x3F
        if sub in TRAP:
            vec = word & 0x1FF
            return Insn(addr, word, TRAP[sub], ["%d" % D, _r(S1), "%d" % vec])
        if sub not in BITFIELD:
            return None
        width = (word >> 5) & 0x1F
        offset = word & 0x1F
        return Insn(addr, word, BITFIELD[sub],
                    [_r(D), _r(S1), "%d<%d>" % (width, offset)])

    # --- triadic register form ---
    if op == 0x3D:
        sub = (word >> 10) & 0x3F
        var = (word >> 5) & 0x1F
        S2 = word & 0x1F
        mn = TRIADIC.get(sub)
        if mn is None:
            return None

        if sub in TRIADIC_NO_OP:
            return Insn(addr, word, mn, [])
        if sub in TRIADIC_ONE_OP:
            return Insn(addr, word, mn, [_r(S2)])
        if sub in TRIADIC_MEM:
            # variant bit 0x10 selects scaled indexing: rS1[rS2]
            if var & VARIANT_SCALED:
                return Insn(addr, word, mn, [_r(D), "%s[%s]" % (_r(S1), _r(S2))])
            return Insn(addr, word, mn, [_r(D), _r(S1), _r(S2)])
        return Insn(addr, word, mn, [_r(D), _r(S1), _r(S2)])

    return None


# Instructions that end a basic block (no fall-through, ignoring delay slots).
TERMINATORS = {"br", "br.n", "jmp", "jmp.n", "rte"}
CALLS = {"bsr", "bsr.n", "jsr", "jsr.n"}
