"""
Loader for BBN nX (TC2000) a.out binaries.

Two things differ from textbook BSD a.out and both matter:

  1. The exec header occupies a full 8192-byte page, so text begins at file
     offset 8192 -- not 0 (classic ZMAGIC) and not 32.
         filesize = 8192 + a_text + a_data + a_syms + strtab
  2. `struct nlist` puts n_strx LAST:
         n_type(1) n_other(1) n_desc(2) n_value(4) n_strx(4)   big-endian

The symbol table is also padded to 12-byte alignment relative to file offset 0,
so the header-derived offset can be a few bytes short of the real one.
"""

import struct

PAGE = 8192
ZMAGIC = 0x010B

N_EXT = 0x01
N_TYPE = 0x1E
N_STAB = 0xE0
TYPE_CHAR = {0x00: "U", 0x02: "A", 0x04: "T", 0x06: "D", 0x08: "B", 0x0A: "I"}


class Sym:
    __slots__ = ("value", "ntype", "name")

    def __init__(self, value, ntype, name):
        self.value, self.ntype, self.name = value, ntype, name

    @property
    def is_text(self):
        return (self.ntype & N_TYPE) == 0x04

    @property
    def letter(self):
        c = TYPE_CHAR.get(self.ntype & N_TYPE, "?")
        return c if self.ntype & N_EXT else c.lower()

    def __repr__(self):
        return "<%08x %s %s>" % (self.value, self.letter, self.name)


class AOut:
    def __init__(self, path):
        self.path = path
        self.data = open(path, "rb").read()
        (self.magic, self.text, self.data_sz, self.bss,
         self.syms, self.entry, self.trsize, self.drsize) = \
            struct.unpack(">8I", self.data[:32])
        if (self.magic & 0xFFFF) != ZMAGIC:
            raise ValueError("%s: not ZMAGIC (magic=%08x)" % (path, self.magic))

        self.textoff = PAGE
        self.dataoff = self.textoff + self.text
        self.symoff = self.dataoff + self.data_sz + self.trsize + self.drsize
        # The string table base follows the *unaligned* symbol offset; only the
        # entry array itself is nudged to 12-byte alignment.  Getting this
        # wrong shifts every name by the pad and silently mangles the table.
        self.stroff = self.symoff + self.syms
        if self.symoff % 12:
            self.symoff += 12 - (self.symoff % 12)

        self.symbols = []
        self.stab_count = 0
        self._load_symbols()

        self._by_addr = sorted(
            ((s.value, s.name) for s in self.symbols if s.is_text))
        self._addrs = [a for a, _ in self._by_addr]
        self.by_name = {s.name: s for s in self.symbols}

    # -- text image ---------------------------------------------------------

    @property
    def text_base(self):
        """Virtual address the text segment loads at."""
        # Derived from the lowest text symbol rather than assumed: nX kernels
        # sit at 0xC0000000 but userland links elsewhere.
        return self._addrs[0] & ~0xFFF if self._addrs else 0

    def text_bytes(self):
        return self.data[self.textoff:self.textoff + self.text]

    def word_at(self, vaddr, base):
        off = self.textoff + (vaddr - base)
        if off < self.textoff or off + 4 > self.textoff + self.text:
            return None
        return struct.unpack(">I", self.data[off:off + 4])[0]

    # -- symbols ------------------------------------------------------------

    def _load_symbols(self):
        d, strbase = self.data, self.stroff
        for i in range(self.syms // 12):
            o = self.symoff + i * 12
            if o + 12 > len(d):
                break
            ntype, _other, _desc, value, strx = struct.unpack(
                ">BbhII", d[o:o + 12])
            if ntype & N_STAB:
                self.stab_count += 1
                continue
            name = self._string(strbase, strx)
            if name:
                self.symbols.append(Sym(value, ntype, name))

    def _string(self, base, strx):
        if strx <= 0:
            return ""
        p = base + strx
        if p >= len(self.data):
            return ""
        e = self.data.find(b"\0", p)
        return self.data[p:e].decode("latin1", "replace") if e > p else ""

    def sym_for(self, addr):
        """Nearest preceding text symbol, as 'name' or 'name+0xNN'."""
        import bisect
        i = bisect.bisect_right(self._addrs, addr) - 1
        if i < 0:
            return None
        a, nm = self._by_addr[i]
        return nm if a == addr else "%s+0x%x" % (nm, addr - a)

    def exact_sym(self, addr):
        import bisect
        i = bisect.bisect_left(self._addrs, addr)
        if i < len(self._addrs) and self._addrs[i] == addr:
            return self._by_addr[i][1]
        return None

    def functions(self):
        """Text symbols as (addr, name, size) using next-symbol as the bound."""
        out = []
        for i, (a, nm) in enumerate(self._by_addr):
            end = self._by_addr[i + 1][0] if i + 1 < len(self._by_addr) else a
            out.append((a, nm, max(0, end - a)))
        return out

    def summary(self):
        return ("%s: text=%d data=%d bss=%d entry=%08x  "
                "%d symbols (%d text), %d stabs" %
                (self.path, self.text, self.data_sz, self.bss, self.entry,
                 len(self.symbols),
                 sum(1 for s in self.symbols if s.is_text), self.stab_count))
