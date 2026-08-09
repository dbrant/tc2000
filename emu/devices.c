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
    if (wmem_hi || stwatch_active) wmem_tick(sub, D, ea);      /* --wmem: watch a VIRTUAL range */
    ea = translate(ea, 0);
    /* Page fault: abort before any memory or register is touched, so the
       instruction can simply be re-executed once the page is there. */
    if (ufault_pending) return;
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
    /* SHA "busy/ready" handshake registers (base+0x08 and base+0x10): the driver
       polls these for bit0 clear -- the controller-ready / doorbell-ack bit --
       before submitting or advancing a command (e.g. _sha_cmd_wait at c00bc148,
       _sha_addrs at c00bebd0).  Our SHA completes every command synchronously, so
       it is always ready; report bit0 clear, else the driver spins forever. */
    if (sysmode && (ea == SHA_BASE + 0x08u || ea == SHA_BASE + 0x10u)
        && (sub == 0x02 || sub == 0x06)) {
        WR(D, mem_r16(ea) & ~1u);
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
            else if (off == CMMU_SAPR || off == CMMU_UAPR)
                tlb_flush();             /* APR changed: drop cached translations */
        }
    }
}

/* Factory-label the SCSI disk: if disk.img block 0 has no valid Sun disklabel,
   write one, so newfs (which only understands "Mach" and "Sun" label styles)
   reads a geometry it accepts instead of "Label style not understood yet".  The
   Sun dk_label (512 bytes, big-endian): magic 0xDABE at 0x1FC, cksum at 0x1FE
   (all 256 shorts must XOR to 0), and the fields newfs actually reads --
   ncyl@0x1B0, nhead@0x1B4, nsect@0x1B6 -- plus the partition map dkl_map[8] at
   0x1BC ({u32 start-cyl, u32 nblk} each; partition b == index 1). */
void sd_ensure_label(void)
{
    if (!disk_img) return;
    u8 lbl[512];
    fseek(disk_img, 0, SEEK_SET);
    if (fread(lbl, 1, 512, disk_img) == 512 &&
        lbl[0x1FC] == 0xDA && lbl[0x1FD] == 0xBE)
        return;                                       /* already labelled */

    fseek(disk_img, 0, SEEK_END);
    long sz = ftell(disk_img);
    u16 nsect = 63, nhead = 16;
    u32 ncyl = (u32)(sz / 512) / ((u32)nsect * nhead);
    if (!ncyl) ncyl = 1;
    u32 blocks = ncyl * nhead * nsect;

    memset(lbl, 0, sizeof lbl);
    snprintf((char *)lbl, 128, "BBN emulated disk cyl %u alt 0 hd %u sec %u",
             ncyl, nhead, nsect);
#define PUT16(o, v) do { lbl[o] = (u8)((v) >> 8); lbl[(o)+1] = (u8)(v); } while (0)
#define PUT32(o, v) do { lbl[o] = (u8)((v) >> 24); lbl[(o)+1] = (u8)((v) >> 16); \
                         lbl[(o)+2] = (u8)((v) >> 8); lbl[(o)+3] = (u8)(v); } while (0)
    PUT16(0x1A4, 3600);                               /* rpm   */
    PUT16(0x1A6, (u16)ncyl);                          /* pcyl  */
    PUT16(0x1B0, (u16)ncyl);                          /* ncyl  */
    PUT16(0x1B2, 0);                                  /* acyl  */
    PUT16(0x1B4, nhead);                              /* nhead */
    PUT16(0x1B6, nsect);                              /* nsect */
    PUT32(0x1BC + 1 * 8 + 0, 0);                      /* b: start cylinder */
    PUT32(0x1BC + 1 * 8 + 4, blocks);                 /* b: block count    */
    PUT32(0x1BC + 2 * 8 + 0, 0);                      /* c: whole disk     */
    PUT32(0x1BC + 2 * 8 + 4, blocks);
    PUT16(0x1FC, 0xDABE);                             /* magic */
    u16 ck = 0;
    for (int i = 0; i < 255; i++) ck ^= (u16)((lbl[i*2] << 8) | lbl[i*2+1]);
    PUT16(0x1FE, ck);
#undef PUT16
#undef PUT32
    fseek(disk_img, 0, SEEK_SET);
    fwrite(lbl, 1, 512, disk_img);
    fflush(disk_img);
    printf("disk image: wrote synthetic Sun disklabel "
           "(%u cyl x %u hd x %u sec = %u blocks)\n", ncyl, nhead, nsect, blocks);
}

/* Stand in for the SHA completion interrupt on the _sdcommand_nowait path.
   The command block (recovered by dumping the live block at c00be6bc):
     +0x00 command/status word (0xc1 on entry; bit5 => copy DMA data out)
     +0x10 DMA buffer PHYSICAL address (data-in destination)
     +0x44 SCSI target number
     +0x6c CDB length (6)
     +0x70.. CDB bytes (big-endian within the words)
     +0x90 transfer length
     +0x68 completion status (bit0 clear on entry => driver's success path)
   For now we model a single direct-access disk at target 0 (routed to
   emu/disk.img); every other target reports empty so sdprobe skips it. */
void sha_sdcomplete(u32 cmd)
{
    u32 target = mem_r32(translate(cmd + 0x44, 0));
    u32 dma    = mem_r32(translate(cmd + 0x10, 0));
    u32 cdbw   = mem_r32(translate(cmd + 0x70, 0));
    u32 xfer   = mem_r32(translate(cmd + 0x90, 0));
    u8  op     = (u8)(cdbw >> 24);
    if (scsi_trace) {
        u32 cw2 = mem_r32(translate(cmd + 0x74, 0));
        u32 lba = (op == 0x08 || op == 0x0a) ? (cdbw & 0x1FFFFFu)
                                             : ((cdbw << 16) | (cw2 >> 16));
        printf("[sdcmd] target=%u op=%02x lba=%u dma=%08x xfer=%u @%llu\n",
               target, op, lba, dma, xfer, (unsigned long long)cpu.count);
    }

    if (target != 0) return;       /* no device: leave block, driver skips */

    /* SCSI block size is 512.  disk.img size gives the capacity. */
    static const u32 SDBLK = 512;
    static u8 buf[0x10000];
    u32 cdbw2 = mem_r32(translate(cmd + 0x74, 0));   /* CDB bytes 4-7 */
    if (xfer > sizeof buf) xfer = sizeof buf;

    switch (op) {
    case 0x00:                     /* TEST UNIT READY: complete good, no data */
        return;
    case 0x12: {                   /* INQUIRY */
        memset(buf, 0, sizeof buf);
        buf[0] = 0x00;             /* peripheral type 0 = direct-access disk */
        buf[1] = 0x00;             /* not removable */
        buf[2] = 0x02;             /* SCSI-2 */
        buf[3] = 0x02;             /* response format */
        buf[4] = 0x1f;             /* additional length (31) */
        memcpy(buf + 8,  "BBN     ", 8);
        memcpy(buf + 16, "EMULATED DISK   ", 16);
        memcpy(buf + 32, "0001", 4);
        u32 n = 36; if (n > xfer) n = xfer;
        for (u32 i = 0; i < n; i++) mem_w8(dma + i, buf[i]);
        return;
    }
    case 0x08:                     /* READ(6):  LBA in CDB[1..3] (21 bits) */
    case 0x28: {                   /* READ(10): LBA in CDB[2..5] (32 bits) */
        u32 lba = (op == 0x08) ? (cdbw & 0x1FFFFFu)
                               : ((cdbw << 16) | (cdbw2 >> 16));
        memset(buf, 0, xfer);
        if (disk_img && dma) {
            if (fseek(disk_img, (long)lba * SDBLK, SEEK_SET) == 0)
                if (fread(buf, 1, xfer, disk_img) == 0) { /* blank tail -> zeros */ }
        }
        if (dma) for (u32 i = 0; i < xfer; i++) mem_w8(dma + i, buf[i]);
        return;
    }
    case 0x0a:                     /* WRITE(6) */
    case 0x2a: {                   /* WRITE(10) */
        u32 lba = (op == 0x0a) ? (cdbw & 0x1FFFFFu)
                               : ((cdbw << 16) | (cdbw2 >> 16));
        if (disk_img && dma) {
            for (u32 i = 0; i < xfer; i++) buf[i] = mem_r8(dma + i);
            if (fseek(disk_img, (long)lba * SDBLK, SEEK_SET) == 0)
                fwrite(buf, 1, xfer, disk_img), fflush(disk_img);
        }
        return;
    }
    case 0x25: {                   /* READ CAPACITY: last LBA (BE) + block size */
        long sz = 0;
        if (disk_img) { fseek(disk_img, 0, SEEK_END); sz = ftell(disk_img); }
        u32 last = sz ? (u32)(sz / SDBLK - 1) : 0x3FFFF;   /* ~128MB fallback */
        if (dma) {
            mem_w8(dma+0, last>>24); mem_w8(dma+1, last>>16);
            mem_w8(dma+2, last>>8);  mem_w8(dma+3, last);
            mem_w8(dma+4, 0); mem_w8(dma+5, 0);
            mem_w8(dma+6, SDBLK>>8); mem_w8(dma+7, SDBLK & 0xff);
        }
        return;
    }
    default:
        return;                    /* other commands: just complete good */
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
