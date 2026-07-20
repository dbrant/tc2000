"""
Broader hunt for the syscall dispatch table: try several record strides and
scan both .text and .data, looking for long runs where a fixed column holds
kernel text addresses.  Const tables are frequently emitted into .text.
"""

import sys
import struct
import bisect

sys.path.insert(0, __file__.rsplit("\\", 1)[0].rsplit("/", 1)[0])
from nxaout import AOut          # noqa: E402

a = AOut(r"e:\git\tc2000\tapeimage\vmunix")
TEXT_LO, TEXT_HI = 0xC0010000, 0xC0010000 + a.text
segs = {
    "text": (TEXT_LO, a.data[8192:8192 + a.text]),
    "data": (0xC1000000, a.data[8192 + a.text:8192 + a.text + a.data_sz]),
}
txt = sorted((s.value, s.name) for s in a.symbols if s.is_text)
taddr = [v for v, _ in txt]


def fname(addr):
    i = bisect.bisect_right(taddr, addr) - 1
    if i < 0:
        return None
    v, n = txt[i]
    return n if v == addr else "%s+0x%x" % (n, addr - v)


def words(buf):
    n = len(buf) // 4
    return struct.unpack(">%dI" % n, buf[:n * 4])


best = []
for name, (base, buf) in segs.items():
    W = words(buf)
    istext = [TEXT_LO <= w < TEXT_HI for w in W]
    for stride in (2, 3, 4):            # in words: 8, 12, 16 bytes
        i = 0
        while i < len(W):
            if istext[i]:
                j, cnt = i, 0
                while j < len(W) and istext[j]:
                    cnt += 1
                    j += stride
                if cnt >= 30:
                    best.append((cnt, name, base, i * 4, stride))
                    i = j
                    continue
            i += 1
best.sort(reverse=True)
print("longest runs of text pointers at fixed stride:")
for cnt, name, base, off, stride in best[:8]:
    print("   %s+%06x  vaddr %08x  stride %d bytes  %d pointers"
          % (name, off, base + off, stride * 4, cnt))

if not best:
    sys.exit("nothing found")

cnt, name, base, off, stride = best[0]
buf = segs[name][1]
W = words(buf)
i0 = off // 4
print("\n=== table at %08x, stride %d bytes, %d entries ==="
      % (base + off, stride * 4, cnt))
INTEREST = {0, 1, 3, 4, 5, 20, 24, 36, 54, 108, 116, 121,
            185, 186, 187, 188, 189, 190}
for k in range(cnt):
    i = i0 + k * stride
    ptr = W[i]
    extra = " ".join("%08x" % W[i + d] for d in range(1, stride))
    mark = "   <<<" if k in INTEREST else ""
    print("  %3d  %-30s  %s%s" % (k, fname(ptr) or "-", extra, mark))
