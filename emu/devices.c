/* Generated split of the former single-file nx88.c.
   Shared types, macros, hot inline accessors, and all
   cross-module declarations live in nx88.h. */
#include "nx88.h"

void memop(u32 sub, u32 D, u32 ea)
{
    /* lda computes an effective ADDRESS only -- it does not access memory, so
       it must return the untranslated virtual address.  Everything else
       translates before touching memory. */
    if (sub >= 0x0C && sub <= 0x0F) { WR(D, ea); return; }
    ea = translate(ea, 0);
    if (scsi_trace && ea >= 0xFC000000u && ea < 0xFC010000u && scsi_trace_n < 100000) {
        int st = (sub >= 0x08 && sub <= 0x0B);
        printf("[scsi] %-2s %08x sub=%x val=%08x pc=%08x @%llu\n",
               st ? "WR" : "RD", ea, sub, st ? RD(D) : 0, dbg_pc,
               (unsigned long long)cpu.count);
        scsi_trace_n++;
    }
    if (sysmode && ea == SHA_CMD && sub == 0x0A) {          /* SHA cmd reg write */
        u16 v = (u16)RD(D);
        if (v == 0x1000) sha_status = 1;                   /* reset: busy   */
        else if (v == 0) sha_status = 2;                   /* pulse done: ready */
        else if (v == 1) {                                 /* command: complete it */
            mem_w16(SHA_BASE + 0x73c, (u16)sha_done_status);
            if (scsi_trace)
                printf("[sha] cmd=1: poked %08x = %04x, reads back %04x\n",
                       SHA_BASE + 0x73c, (u16)sha_done_status,
                       mem_r16(SHA_BASE + 0x73c));
        }
        mem_w16(ea, v);
        return;
    }
    if (sysmode && ea == SHA_STATUS && (sub == 0x02 || sub == 0x06)) { /* status read */
        WR(D, sub == 0x06 ? (u32)(s32)(int16_t)sha_status : sha_status);
        return;
    }
    /* The free-running timer at 0xE07E8018 must respond to sub-word reads too:
       _startclock_duart polls it with ld.h and spins until it changes. */
    if (sysmode && (ea & ~3u) == TIMER_ADDR) {
        u32 t = (u32)(cpu.count * tick_scale);   /* 32-bit big-endian counter */
        u32 off = ea & 3;
        switch (sub) {
        case 0x05: WR(D, t); return;
        case 0x02: WR(D, (t >> (off < 2 ? 16 : 0)) & 0xFFFF); return;   /* ld.hu */
        case 0x06: { u16 h = (t >> (off < 2 ? 16 : 0)) & 0xFFFF;
                     WR(D, (s32)(int16_t)h); return; }                  /* ld.h  */
        case 0x03: WR(D, (t >> (24 - off * 8)) & 0xFF); return;         /* ld.bu */
        case 0x07: { u8 b = (t >> (24 - off * 8)) & 0xFF;
                     WR(D, (s32)(int8_t)b); return; }                   /* ld.b  */
        }
    }
    switch (sub) {
    case 0x05: WR(D, dev_read32(ea)); break;                     /* ld    */
    case 0x07: { u8 v = mem_r8(ea);  WR(D, (s32)(int8_t)v); } break;   /* ld.b  */
    case 0x03: WR(D, mem_r8(ea)); break;                         /* ld.bu */
    case 0x06: { u16 v = mem_r16(ea); WR(D, (s32)(int16_t)v); } break; /* ld.h */
    case 0x02: WR(D, mem_r16(ea)); break;                        /* ld.hu */
    case 0x04: WR(D, dev_read32(ea)); WR(D + 1, dev_read32(ea + 4)); break; /* ld.d */
    case 0x09: dev_write32(ea, RD(D)); break;                     /* st    */
    case 0x0B: mem_w8(ea, (u8)RD(D)); tcs_poke(ea); break;       /* st.b  */
    case 0x0A: mem_w16(ea, (u16)RD(D)); break;                   /* st.h  */
    case 0x08: dev_write32(ea, RD(D)); dev_write32(ea + 4, RD(D + 1)); break; /* st.d */
    case 0x01: { u32 old = mem_r32(ea); mem_w32(ea, RD(D)); WR(D, old); } break;
    case 0x00: { u8  old = mem_r8(ea);  mem_w8(ea, (u8)RD(D)); WR(D, old); } break;
    }
}

int is_cmmu_id(u32 a)
{
    for (int i = 0; i < n_cmmu; i++) if (cmmu_present[i] == a) return 1;
    return 0;
}

u32 cmmu_id_for(u32 a)
{
    u32 n = (a >> 12) & 0xFF;
    return (n << 24) | 0x00A00000u;
}

u32 dev_read32(u32 a)
{
    if (sysmode) {
        if (a == TIMER_ADDR) return (u32)(cpu.count * tick_scale);
        if (is_cmmu_id(a))   return cmmu_id_for(a);
        if (a == CMRAM_DATA) { cmram_reads++; return cmram_r32(cmram_sel & ~3u); }
        if (a == IRQ_SOURCE_REG) return irq_source;   /* pending interrupt src */
        if (a == 0xE07EA008u || a == 0xE07EA010u || a == 0xE07EA028u)
            return 0;                    /* status ports: ready, no error */
    }
    return mem_r32(a);
}

void tcs_poke(u32 a)
{
    if (a != tcs_mbox_pa) return;
    u8 cmd = mem_r8(tcs_mbox_pa);
    if (!cmd) return;
    if (tcs_trace)
        printf("[tcs] command %02x (args %08x %08x) -> ok\n", cmd,
               mem_r32(tcs_mbox_pa + 0x10), mem_r32(tcs_mbox_pa + 0x14));
    mem_w8(tcs_mbox_pa, 0);         /* consumed */
    mem_w8(tcs_mbox_pa + 5, 1);     /* response ready, no error */
    tcs_commands++;
}

int cmmu_base_of(u32 a, u32 *base)
{
    for (int i = 0; i < n_cmmu; i++)
        if (a >= cmmu_present[i] && a < cmmu_present[i] + 0x1000) {
            *base = cmmu_present[i];
            return 1;
        }
    return 0;
}

void dev_write32(u32 a, u32 v)
{
    if (sysmode) {
        {
            u32 cb;
            if (batc_trace && cmmu_base_of(a, &cb) && (a - cb) >= 0x400 && (a - cb) < 0x500)
                printf("[batc] %08x off=0x%x <- %08x\n", a, a - cb, v);
        }
        if (a == CMRAM_SELECT) { cmram_sel = v; return; }
        if (a == CMRAM_DATA) { cmram_w32(cmram_sel & ~3u, v); cmram_writes++; return; }
        if (a == IRQ_SOURCE_REG) { irq_source = v; return; } /* ack clears src */
    }
    mem_w32(a, v);
    if (sysmode) {
        tcs_poke(a);
        u32 base;
        if (cmmu_base_of(a, &base)) {
            u32 off = a - base;
            if (off == CMMU_SCR) cmmu_command(base, v);
            else if (off == CMMU_SAPR || off == CMMU_UAPR) {
                tlb_flush();             /* APR changed: drop cached translations */
                if (mmu_trace)
                    printf("[cmmu] %08x %s <- %08x   (pc=%08x)\n", base,
                           off == CMMU_SAPR ? "SAPR" : "UAPR", v, cpu.pc);
            }
        }
    }
}

/* Process a pending SHA command.  The driver blocked in tsleep waiting for the
   completion interrupt handler (_shaintr) to fill in its command descriptor;
   since we short-circuit the wait, we stand in for _shaintr and mark the
   command complete.  The descriptor is in r27 for the SHA callers; [desc+4]
   bit 2 is the error flag -- clear it so the driver takes its success path. */
void sha_complete(void)
{
    u32 desc = RD(27);
    if (desc >= 0xC0000000u) {
        u32 pa = translate(desc + 4, 0);
        /* Bit 2 is the error flag.  Bit 4 is the "command still outstanding"
           flag shareset tests the instant it wakes (`ld r6,[r27+4]; bb0 4`);
           leaving it set is what produced "SHA_WORKQ_INIT still asserted" and
           stopped the target-probe loop before sdprobe ever ran. */
        mem_w32(pa, mem_r32(pa) & ~sha_desc_clear);
    }
    if (scsi_trace) {
        printf("[sha] complete: caller=%08x desc=%08x r4(lock?)=%08x chan=%08x @%llu\n",
               RD(1), desc, RD(4), RD(2),
               (unsigned long long)cpu.count);
        /* dump the IOPB descriptor and the SHA dual-port CDB region */
        if (desc >= 0xC0000000u) {
            printf("[iopb] desc %08x:", desc);
            for (int i = 0; i < 0x40; i += 4)
                printf(" %08x", mem_r32(translate(desc + i, 0)));
            printf("\n");
        }
        printf("[dpram] fc008890:");
        for (u32 aa = 0xFC008890u; aa < 0xFC0088C0u; aa += 2)
            printf(" %04x", mem_r16(aa));
        printf("\n");
    }
}
