/* Generated split of the former single-file nx88.c.
   Shared types, macros, hot inline accessors, and all
   cross-module declarations live in nx88.h. */
#include "nx88.h"

/* Read a NUL-terminated string out of user memory FOR THE EMULATOR's own use.
   ★ It must not disturb the CPU's fault state.  translate() records a page
   fault on a miss, and leaving that set makes the very next instruction bail
   and deliver a fault for an address the guest never touched.  That is exactly
   what wedged init: its child open()s a path whose page is not resident, the
   sd0-device check peeked at it, the peek faulted, the path came back EMPTY,
   and the kernel then spun forever in a namei directory scan.  Snapshot the
   fault state, stop the string at the first byte that is not resident, and put
   the state back -- the kernel will fault the page in properly on its own. */
int uread_str(u32 va, char *buf, size_t n)
{
    int save_pending = ufault_pending, save_code = ufault_code;
    int save_write = ufault_write;
    u32 save_va = ufault_va, save_pc = ufault_pc, save_width = ufault_width;
    size_t i = 0;
    for (; i + 1 < n; i++) {
        ufault_pending = 0;
        u32 pa = translate(va + (u32)i, 0);
        if (ufault_pending || !pa) break;          /* not resident -- stop */
        buf[i] = (char)mem_r8(pa);
        if (!buf[i]) break;
    }
    buf[i] = 0;
    ufault_pending = save_pending; ufault_code = save_code;
    ufault_write = save_write;     ufault_va = save_va;
    ufault_pc = save_pc;           ufault_width = save_width;
    return (int)i;
}

/* Copy bytes INTO user memory for an emulator-serviced syscall, fault-safely.
   Same hazard as uread_str: translate() records a page fault on a miss, so the
   naive loop wrote to physical 0 and left a bogus fault pending for the next
   instruction.  Stop at the first byte that is not resident and report how many
   were actually transferred -- a short read is legal, a corrupted fault state
   is not. */
u32 uwrite_mem(u32 va, const u8 *src, u32 n)
{
    int save_pending = ufault_pending, save_code = ufault_code;
    int save_write = ufault_write;
    u32 save_va = ufault_va, save_pc = ufault_pc, save_width = ufault_width;
    u32 i = 0;
    for (; i < n; i++) {
        ufault_pending = 0;
        u32 pa = translate(va + i, 0);
        if (ufault_pending || !pa) break;
        mem_w8(pa, src[i]);
    }
    ufault_pending = save_pending; ufault_code = save_code;
    ufault_write = save_write;     ufault_va = save_va;
    ufault_pc = save_pc;           ufault_width = save_width;
    return i;
}

/* ★ Make a user buffer resident before an emulator-serviced syscall touches it.
   translate() records a page fault on a miss and returns 0 -- but only for the
   FIRST missing byte.  Once ufault_pending is set it stops taking that branch
   and falls through to `return cphys(va)`, which for a low user VA is the
   IDENTITY map.  So the naive `mem_w8(translate(va + i, 0), ...)` loop that the
   /hosttar and raw-disk paths used sprayed their data straight into PHYSICAL
   memory from the first unmapped byte onward.

   That is what broke `tar xpf /hosttar`: tar read 0x2800 bytes into an
   unmapped 0x12000, the emulator wrote them to physical 0x12000-0x14800, and
   physical 0x14710 is the kernel's own ON-FAULT recovery pointer (the global at
   VA 0xC0014710 that exreturn checks before resolving any data fault).  With it
   holding archive bytes instead of 0, the next user fault was abandoned and
   longjmp'd through a garbage pcb -- "jumped to 00000000 from kernel pc=..".
   The corruption landed ~250k instructions before the symptom, in a completely
   different subsystem, which is why it looked like a scheduler bug.

   Touch every page first.  If one is missing, leave the fault pending for
   run_sys to deliver and point the resume PC at the syscall instruction so the
   whole syscall re-runs once the kernel has paged it in.  Returns 1 if the
   caller must give up and retry. */
int ubuf_fault(u32 va, u32 len, int forwrite, u32 tpc)
{
    if (!len) return 0;
    int save = ufault_pending;
    for (u32 p = va & ~0xFFFu; p != ((va + len - 1) & ~0xFFFu) + 0x1000u;
         p += 0x1000u) {
        ufault_pending = 0;
        u32 pa = translate(p, 0);
        if (ufault_pending || !pa) {
            ufault_pending = 1;
            ufault_va    = p;
            ufault_code  = 0;
            ufault_write = forwrite;
            ufault_width = 4;
            ufault_pc    = tpc;
            cpu.pc       = tpc;      /* re-execute the syscall after the fault */
            return 1;
        }
    }
    ufault_pending = save;
    return 0;
}
