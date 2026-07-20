"""
Parse the stabs debug entries in vmunix and use them as an independent check on
the regular symbol table.

The two tables are produced by different parts of the toolchain, so where they
disagree one of them is wrong -- and stabs carry enough context (source file,
function type, register-save masks) to say which.

Stab types of interest:
    0x64  N_SO    source file name; value = address the module starts at
    0x84  N_SOL   included file
    0x24  N_FUN   function; string is "name:F<type>", value = address
    0x26  N_STSYM static data
    0x28  N_LCSYM bss
    0x20  N_GSYM  global
"""

import sys
import struct
import collections

sys.path.insert(0, __file__.rsplit("\\", 1)[0].rsplit("/", 1)[0])
from nxaout import AOut          # noqa: E402

N_SO, N_SOL, N_FUN, N_GSYM, N_STSYM, N_LCSYM = 0x64, 0x84, 0x24, 0x20, 0x26, 0x28

a = AOut(r"e:\git\tc2000\tapeimage\vmunix")
d = a.data
print(a.summary())

stabs = []
for i in range(a.syms // 12):
    o = a.symoff + i * 12
    ntype, other, desc, val, strx = struct.unpack(">BbhII", d[o:o + 12])
    if not (ntype & 0xE0):
        continue
    stabs.append((i, ntype, desc, val, a._string(a.stroff, strx)))

print("stab entries: %d" % len(stabs))
print("\ntype histogram:")
for t, n in collections.Counter(s[1] for s in stabs).most_common(12):
    print("   0x%02x  %6d" % (t, n))

# ---- N_FUN: independent function name -> address map --------------------
funs = {}
for _, ntype, _desc, val, s in stabs:
    if ntype == N_FUN and s:
        name = s.split(":", 1)[0]
        if name:
            funs.setdefault(val, name)
print("\nN_FUN entries: %d distinct addresses" % len(funs))

# ---- N_SO: module -> address --------------------------------------------
sos = sorted((val, s) for _, ntype, _d, val, s in stabs
             if ntype == N_SO and s and val)
print("N_SO source files: %d" % len(sos))
for val, s in sos[:8]:
    print("   %08x  %s" % (val, s))


def module_of(addr):
    import bisect
    keys = [v for v, _ in sos]
    i = bisect.bisect_right(keys, addr) - 1
    return sos[i][1] if i >= 0 else "?"


# ---- cross-check ---------------------------------------------------------
print("\n" + "=" * 70)
print("CROSS-CHECK: regular symbol table vs N_FUN stabs")
agree = disagree = only_sym = 0
bad = []
for s in a.symbols:
    if not s.is_text:
        continue
    st = funs.get(s.value)
    if st is None:
        only_sym += 1
        continue
    # stabs names are unmangled (no leading underscore)
    if st == s.name or ("_" + st) == s.name:
        agree += 1
    else:
        disagree += 1
        bad.append((s.value, s.name, st))
print("  agree     : %d" % agree)
print("  disagree  : %d" % disagree)
print("  no stab   : %d" % only_sym)
if bad:
    print("\n  disagreements (addr, symtab says, stabs say, module):")
    for val, nm, st in sorted(bad)[:25]:
        print("    %08x  %-28s %-28s %s" % (val, nm, st, module_of(val)))

# ---- the specific cases in question --------------------------------------
print("\n" + "=" * 70)
print("SPECIFIC CASES")
for probe in (0xC0018138, 0xC009EFD0, 0xC009EFE8, 0xC009A600, 0xC0099E2C):
    print("  %08x  symtab=%-24s stabs=%-24s module=%s" %
          (probe,
           a.exact_sym(probe) or "-",
           funs.get(probe, "-"),
           module_of(probe)))

for want in ("_bzero", "_getblk", "_ttyoutput", "_paddr2intrlvpnode"):
    s = a.by_name.get(want)
    rev = [v for v, n in funs.items() if ("_" + n) == want or n == want]
    print("  %-20s symtab_value=%s   stabs_value=%s" %
          (want,
           "%08x" % s.value if s else "-",
           ", ".join("%08x" % v for v in rev) or "-"))
