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
