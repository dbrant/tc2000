/* Generated split of the former single-file nx88.c.
   Shared types, macros, hot inline accessors, and all
   cross-module declarations live in nx88.h. */
#include "nx88.h"

u32 do_cmp(u32 a, u32 b)
{
    s32 sa = (s32)a, sb = (s32)b;
    u32 v = 1u << 1;
    v |= (a == b) ? (1u << C_EQ) : (1u << C_NE);
    if (sa >  sb) v |= 1u << C_GT;
    if (sa <= sb) v |= 1u << C_LE;
    if (sa <  sb) v |= 1u << C_LT;
    if (sa >= sb) v |= 1u << C_GE;
    if (a  >  b)  v |= 1u << C_HI;
    if (a  <= b)  v |= 1u << C_LS;
    if (a  <  b)  v |= 1u << C_LO;
    if (a  >= b)  v |= 1u << C_HS;
    return v;
}

int cond_true(u32 m5, u32 a)
{
    s32 s = (s32)a;
    switch (m5) {
    case 0x01: return s >  0;
    case 0x02: return s == 0;
    case 0x03: return s >= 0;
    case 0x0C: return s <  0;
    case 0x0D: return s != 0;
    case 0x0E: return s <= 0;
    default:   return 0;
    }
}

void bitfield(u32 sub, u32 D, u32 a, u32 width, u32 offset)
{
    u32 w = width ? width : 32;
    u32 m = (w >= 32) ? 0xFFFFFFFFu : ((1u << w) - 1);
    switch (sub) {
    case 0x26: WR(D, (a >> offset) & m); break;                 /* extu */
    case 0x24: {                                                 /* ext  */
        u32 v = (a >> offset) & m;
        if (w < 32 && (v & (1u << (w - 1)))) v |= ~m;
        WR(D, v); break;
    }
    case 0x28: WR(D, (a & m) << offset); break;                  /* mak  */
    case 0x20: WR(D, a & ~(m << offset)); break;                 /* clr  */
    case 0x22: WR(D, a | (m << offset)); break;                  /* set  */
    case 0x2A: { u32 o = offset & 31;
                 WR(D, o ? ((a >> o) | (a << (32 - o))) : a); break; } /* rot */
    }
}

/*
 * MC88100 exception/interrupt delivery.
 *
 * The RTE side already exists (restore PSR from EPSR, resume at SNIP).  This is
 * the entry side: save PSR->EPSR and the resume pointer into SXIP/SNIP/SFIP,
 * put the CPU in the exception state (supervisor, interrupts disabled, shadow
 * frozen), and vector to VBR + vector*8 -- where the kernel's vector table
 * (an 8-byte `nop; br handler` per vector) sends control to the real handler.
 * Interrupts are checked between instructions (never mid-delay-slot), so the
 * resume point is simply the not-yet-executed instruction at cpu.pc.
 */
void deliver_exception(u32 vector)
{
    cpu.cr[2] = cpu.cr[1];                  /* EPSR = PSR                     */
    /* Interrupt taken between instructions: the execute stage is empty, so
       SXIP is INVALID (V=bit1 clear) and the resume point is SNIP.  Marking
       SXIP invalid is what lets the kernel's saveregs (which nulls SNIP and
       points SFIP at _hardclock_interface) fall through to SFIP on its rte. */
    cpu.cr[4] = cpu.pc;                      /* SXIP = pc, invalid (V=0)       */
    cpu.cr[5] = cpu.pc | 2u;                /* SNIP = resume pc, valid        */
    cpu.cr[6] = (cpu.pc + 4u) | 2u;         /* SFIP valid                     */
    cpu.cr[1] = cpu.cr[1] | 0x80000003u;    /* supervisor | IND | SFRZ        */
    cpu.pc = cpu.cr[7] + vector * 8u;       /* VBR + vector*8                 */
}

/* Deliver a synchronous TRAP (a tb0/tb1/tcnd trap instruction, a page fault,
   etc.) to the kernel.  Unlike an interrupt, a trap is caused BY an instruction,
   so the execute stage is occupied: SXIP points at the trapping instruction and
   is VALID.  The kernel's saveregs reads cr4/cr5/cr6; the syscall path advances
   the return PC past the trap (nX: SXIP+4 on error, SXIP+8 on success) before
   its rte.  Vector to VBR + vector*8 like any exception. */
void deliver_trap(u32 vector, u32 tpc)
{
    cpu.cr[2] = cpu.cr[1];                  /* EPSR = PSR at trap time         */
    /* Match the interrupt path (which is known to work through the shared
       saveregs trampoline): SXIP INVALID so saveregs' rte falls through to its
       SFIP continuation; the trap PC lives in SNIP (valid) as the resume base
       the syscall dispatch adjusts (+4 err / +8 ok). */
    cpu.cr[4] = tpc;                        /* SXIP = tpc, INVALID (V=0)       */
    /* The trapping instruction HAS executed, so the resume point is the NEXT
       instruction: SNIP = tpc+4 (valid), SFIP = tpc+8.  This is what makes the
       nX syscall convention land correctly -- the handler returns to SNIP on
       error (tpc+4) and SNIP+4 on success (tpc+8).  Setting SNIP = tpc instead
       re-executes the trap forever. */
    cpu.cr[5] = (tpc + 4u) | 2u;            /* SNIP = next instruction, valid  */
    cpu.cr[6] = (tpc + 8u) | 2u;            /* SFIP valid                      */
    cpu.cr[1] = cpu.cr[1] | 0x80000003u;    /* supervisor | IND | SFRZ         */
    cpu.pc = cpu.cr[7] + vector * 8u;       /* VBR + vector*8                  */
    cpu.has_pending = 0;                    /* abandon any pending delay slot  */
}

/* MC88100 floating point (SFU1, opcode 0x21).  Precision codes: 0=single
   (one register), 1=double (register pair rn:rn+1, high word in rn), 2=extended
   (unused by nX -- treated as double).  Values are carried internally as C
   double.  This code is reached first by the scheduler's load-average update
   (_add_curproc), which is statistical and does not gate control flow, so exact
   rounding is not critical -- correctness of not trapping is what matters. */
double fp_read(u32 reg, int prec)
{
    if (prec == 0) {
        u32 b = RD(reg); float f; memcpy(&f, &b, 4); return (double)f;
    }
    u64 b = ((u64)RD(reg) << 32) | RD(reg + 1);
    double d; memcpy(&d, &b, 8); return d;
}

void fp_write(u32 reg, int prec, double v)
{
    if (prec == 0) {
        float f = (float)v; u32 b; memcpy(&b, &f, 4); WR(reg, b); return;
    }
    u64 b; memcpy(&b, &v, 8);
    WR(reg, (u32)(b >> 32)); WR(reg + 1, (u32)b);
}

/* Execute one instruction.  Returns 1 on trap. */
int step(void)
{
    u32 pc = cpu.pc;
    dbg_pc = pc; dbg_count = cpu.count;

    /* SHA I/O wait: complete synchronously and return from tsleep as if woken.
       sleep_and_unlock releases the caller's lock (3rd arg, r4) on the normal
       wakeup path; release it here too, but only when r4 is a kernel-space lock
       (0xC0000000-0xE0000000) -- not every SHA wait passes a lock, and a device
       register must never be written 0.  Simple unlock writes 0. */
    if (sha_sync && sysmode && pc == TSLEEP_ENTRY
        && RD(1) >= SHA_O_LO && RD(1) < SHA_O_HI) {
        sha_complete();
        if (RD(4) >= 0xC0000000u && RD(4) < 0xE0000000u)
            mem_w32(translate(RD(4), 0), 0);   /* release the sleep lock */
        cpu.pc = RD(1);            /* return to the SHA caller            */
        WR(2, 0);                  /* tsleep result 0 = woken / success   */
        return 0;
    }

    /* _sdcommand_nowait (c00be6bc) issues a SCSI command to the SHA and then
       blocks via swtch_pri(-1) waiting for the completion interrupt to re-queue
       it.  We model no interrupt, so it would deadlock -- and with nothing else
       runnable the scheduler panics `unix_switch: NULL`.  Stand in for the
       completion: execute the SCSI command against our modelled target, mark the
       command block done, and return to the post-wait code (c00be6f0) instead of
       switching.  Detected at swtch_pri's entry (r1 = the return into
       _sdcommand_nowait, r27 = the command block). */
    if (sha_sync && sysmode && pc == 0xC0055CC0u && RD(1) == 0xC00BE6F0u) {
        sha_sdcomplete(RD(27));
        cpu.pc = 0xC00BE6F0u;
        return 0;
    }

    /* A third SHA completion-wait shape: a poll loop (e.g. c00bc134-c00bc16c)
       spins on the command block's completion flag [r27+0x68] bit0, sleeping via
       _slaves_remaining between reads, waiting for the interrupt handler to set
       it.  On the first poll with the bit clear, stand in for the completion:
       execute the SCSI command and set the flag so the loop exits. */
    if (sha_sync && sysmode && pc == 0xC00BC148u) {
        u32 pa = translate(RD(27) + 0x68, 0);
        if (!(mem_r32(pa) & 1u)) {
            sha_sdcomplete(RD(27));
            mem_w32(pa, mem_r32(pa) | 1u);
        }
    }
    /* _cause_tlbflush (c00a7994) is a cross-CPU TLB shootdown: it spins at
       c00a79e4 waiting for the per-node ack counter [r8] (vv(0xc0014040)) to
       reach r27 = the number of CPUs that must acknowledge.  On our single CPU
       no other processor ever bumps it, so newfs (which flushes when it maps the
       raw disk buffers) hangs here.  A uniprocessor flush is local and instant --
       force the counter to the target so the wait completes immediately. */
    if (sysmode && pc == 0xC00A79E4u) {
        u32 pa = translate(RD(8), 0);
        if (mem_r32(pa) < RD(27)) mem_w32(pa, RD(27));
    }

    /* _copy_out_buffer (c00be4c0): a scatter-gather DMA loop that issues
       _multphysio per SG segment and waits at tsleep (c0054a64, return c00be520)
       for the segment -- a REAL sleep just past the SHA tsleep-intercept range,
       so it panics `sleep_and_unlock` in idle context.  Stand in for the SG
       completion: run the SCSI transfer and set the in-progress marker
       [r27+0x24] (0xffff => keep looping) to 0 so the loop exits, then return
       from the sleep without switching. */
    if (sha_sync && sysmode && pc == TSLEEP_ENTRY && RD(1) == 0xC00BE520u) {
        sha_sdcomplete(RD(27));
        mem_w16(translate(RD(27) + 0x24, 0), 0);
        if (RD(4) >= 0xC0000000u && RD(4) < 0xE0000000u)
            mem_w32(translate(RD(4), 0), 0);
        cpu.pc = RD(1);
        WR(2, 0);
        return 0;
    }

    /* b2vme slave-map allocator (c00adc5c): pops a VME-window entry off a tiny
       per-node free list (head at vv(0xc0014080), chain table at 0xf4000800),
       panicking `vme_slave_map_alloc: node %d` when the head hits the 0x1ff
       exhausted-sentinel.  The real driver frees each window after its DMA
       completes (via the interrupt), replenishing the list; we complete every
       DMA synchronously, so no window is ever in flight and the pool can be
       recycled freely.  When the head is exhausted, reset it to entry 0 so the
       (preserved) chain hands out entries 0,1 again. */
    if (sysmode && pc == 0xC00ADCE0u) {
        u32 pa = translate(RD(24), 0);
        if ((mem_r32(pa) & 0x1FFu) == 0x1FFu) mem_w32(pa, 0);
    }

    /* _sha_cmd_wait (c00bd498) waits for a free SHA command-QUEUE SLOT before
       writing the next command: it polls the slot halfword (r1 = SHA dual-port
       base+0x1c + idx*0xc) for bit0 CLEAR.  The controller clears that bit when
       it finishes a slot's command; we complete commands synchronously but never
       ran the interrupt that frees the slot, so the queue fills and sdprobe
       stalls at target 4/5.  Free the slot here so the driver can reuse it. */
    if (sha_sync && sysmode && pc == 0xC00BD498u) {
        u32 pa = translate(RD(1), 0);
        u16 v = mem_r16(pa);
        if (v & 1u) mem_w16(pa, v & ~1u);
    }

    /* Disk strategy.  Satisfying I/O at the biowait below covers only the
       SYNCHRONOUS case, and the buffer cache does not read that way for long:
       a sequential read issues a B_ASYNC read-ahead, whose completion is
       supposed to arrive as an interrupt running biodone -- which, for an async
       buffer, is also what calls brelse and puts it back.  With no interrupt,
       each read-ahead buffer stayed B_BUSY forever, and after a handful getblk
       found one busy and slept: `cat' died on any file over about three blocks.

       So complete the transfer here, at the driver's front door, and then jump
       into the kernel's own _biodone with the buffer -- letting it do the real
       bookkeeping, wakeup and brelse, for sync and async alike. */
    if (biowait_sync && sysmode
        && ((pc == SDSTRATEGY) || (pc == TSLEEP_ENTRY && RD(1) == GETBLK_WAIT))) {
        u32 buf = RD(2);
        if (buf >= 0xC0000000u) {
            u32 flags  = mem_r32(translate(buf + 0x00, 0));
            u32 bcount = mem_r32(translate(buf + 0x14, 0));
            u32 addr   = mem_r32(translate(buf + 0x3c, 0));
            u32 blkno  = mem_r32(translate(buf + 0x40, 0));
            if (root_img && (flags & 1u) && addr >= 0xC0000000u
                && bcount && bcount <= 0x10000u) {
                u8 tmp[0x10000];
                if (fseek(root_img, (long)blkno * 512, SEEK_SET) == 0) {
                    size_t got = fread(tmp, 1, bcount, root_img);
                    for (size_t k = got; k < bcount; k++) tmp[k] = 0;
                    for (u32 k = 0; k < bcount; k += 4)
                        mem_w32(translate(addr + k, 0), be32(tmp + k));
                }
            }
            if (scsi_trace)
                printf("[%s] buf=%08x flags=%08x blk=%u bcount=%u addr=%08x @%llu\n",
                       pc == SDSTRATEGY ? "strategy" : "getblk-stuck",
                       buf, flags, blkno, bcount, addr,
                       (unsigned long long)cpu.count);
            if (pc != SDSTRATEGY) {
                /* Standing in for the completion interrupt getblk is waiting
                   for: release the lock the sleep would have dropped, and
                   arrange for biodone to return where the sleep would have. */
                if (RD(4) >= 0xC0000000u && RD(4) < 0xE0000000u)
                    mem_w32(translate(RD(4), 0), 0);
                WR(1, GETBLK_WAIT);
            }
            cpu.pc = BIODONE;          /* biodone(bp) -- r2 is already the bp */
            return 0;
        }
    }

    /* biowait completion.  The buffer-cache wait loop in _brelvp (c0074098)
       spins `while (!(bp->b_flags & B_DONE)) sleep(bp)`; bit1 of bp->b_flags
       (r2) is B_DONE (0x02).  The disk I/O was issued to the SHA and completed
       synchronously by our intercept above, but nothing ran biodone() on the
       buffer, so B_DONE never gets set and the wait sleeps -- panicking because
       we run in idle-proc context.  Stand in for biodone: mark the buffer done
       and return without sleeping so the wait loop exits.  Gated on the exact
       caller PC (the sleep call inside the biowait loop). */
    if (biowait_sync && sysmode && pc == TSLEEP_ENTRY && RD(1) == 0xC00740E8u) {
        u32 buf = RD(2);
        if (buf >= 0xC0000000u) {
            /* struct buf fields (offsets recovered by dumping the live buffer):
               +0x00 b_flags, +0x14 b_bcount, +0x38 b_pblk/DMA, +0x3c b_un.b_addr
               (kernel VA), +0x40 b_blkno (512-byte units).  Satisfy a READ by
               copying tapeimage.img[b_blkno*512 : +b_bcount] into b_un.b_addr;
               this is what the SHA DMA + biodone would have left. */
            u32 flags  = mem_r32(translate(buf + 0x00, 0));
            u32 bcount = mem_r32(translate(buf + 0x14, 0));
            u32 addr   = mem_r32(translate(buf + 0x3c, 0));
            u32 blkno  = mem_r32(translate(buf + 0x40, 0));
            int is_read = (flags & 1u);             /* B_READ = 0x01 */
            if (root_img && is_read && addr >= 0xC0000000u
                && bcount && bcount <= 0x10000u) {
                u8 tmp[0x10000];
                if (fseek(root_img, (long)blkno * 512, SEEK_SET) == 0) {
                    size_t got = fread(tmp, 1, bcount, root_img);
                    for (size_t k = got; k < bcount; k++) tmp[k] = 0;
                    for (u32 k = 0; k < bcount; k += 4)
                        mem_w32(translate(addr + k, 0), be32(tmp + k));
                    if (scsi_trace)
                        printf("[diskread] blk=%u (off=0x%lx) bcount=%u -> %08x got=%zu\n",
                               blkno, (long)blkno * 512, bcount, addr, got);
                }
            }
            u32 pa = translate(buf, 0);
            mem_w32(pa, mem_r32(pa) | 2u);         /* set B_DONE (bit 1) */
            if (scsi_trace)
                printf("[biodone] buf=%08x flags=%08x->%08x blk=%u bcount=%u addr=%08x @%llu\n",
                       buf, flags, mem_r32(pa), blkno, bcount, addr,
                       (unsigned long long)cpu.count);
        }
        /* The real sleep_and_unlock releases the lock passed in r4 (buf+0x1c)
           as it sleeps; we must too, or the biowait loop's simple_lock at the
           top spins on a lock we still hold. */
        if (RD(4) >= 0xC0000000u && RD(4) < 0xE0000000u)
            mem_w32(translate(RD(4), 0), 0);
        cpu.pc = RD(1);
        WR(2, 0);
        return 0;
    }

    /* synchrtc() (c009ba14): cross-node RTC synchronization.  The kernel counts
       64 nodes (the node-config device reads all slots present -- see the
       node-presence note) and loops synchronizing each node's real-time clock,
       waiting ~20M timer ticks for a per-node vv variable to reach -1.  The 63
       phantom nodes never respond, so it times out and panics "synchrtc".  On a
       single emulated node cross-node RTC sync is meaningless; skip the whole
       routine (its local timer keeps running).  Making the machine truly
       1-node instead (--nodes=1) avoids this but surfaces the idle-proc _sleep
       assertion far earlier, so skipping is the lighter fix. */
    if (skip_synchrtc && sysmode && pc == 0xC009BA14u) {
        cpu.pc = RD(1);
        WR(2, 0);
        return 0;
    }


    /* one-shot: dump the run queue + curproc context at the swapper sleep */
    if (uland_probe && sysmode && pc == 0xC0054720u && RD(1) == 0xC004859Cu) {
        static int once = 0;
        if (!once) { once = 1;
            u32 cp = mem_r32(translate(0xFBFFE0F0u, 0));
            u32 ctx = mem_r32(translate(cp + 0xc8, 0));
            printf("[uland] curproc=%08x link+4=%08x state+40=%08x ctx+c8=%08x\n",
                   cp, mem_r32(translate(cp+0x04,0)), mem_r32(translate(cp+0x40,0)), ctx);
            if (ctx >= 0xC0000000u)
                printf("        ctx: pc+80=%08x sp+7c=%08x pgdir+a0=%08x\n",
                       mem_r32(translate(ctx+0x80,0)), mem_r32(translate(ctx+0x7c,0)),
                       mem_r32(translate(ctx+0xa0,0)));
            /* dump the run-queue head region (c0014498) and follow the chain */
            printf("        runq head c0014498: ");
            for (int i=0;i<8;i++) printf("%08x ", mem_r32(translate(0xC0014498u+i*4,0)));
            printf("\n");
            u32 link = mem_r32(translate(0xC0014498u, 0));   /* first entry */
            for (int n=0; n<12 && link>=0xC0000000u && link!=0xC0014498u; n++) {
                printf("        rq[%d] proc=%08x  fwd+0=%08x  ctx+c8=%08x pc=%08x\n",
                       n, link, mem_r32(translate(link+0,0)),
                       mem_r32(translate(link+0xc8,0)),
                       mem_r32(translate(link+0xc8,0))>=0xC0000000u
                         ? mem_r32(translate(mem_r32(translate(link+0xc8,0))+0x80,0)) : 0);
                u32 nx = mem_r32(translate(link+0, 0));
                if (nx==link) break;
                link = nx;
            }
        }
    }

    /* debug: pipeline state at load_context's rte -- only for switches that
       land in USER mode (EPSR bit31 clear); the 64 boot switches are all
       supervisor and would just be noise. */
    if (trace_traps && sysmode && pc == 0xC00175C0u && !(cpu.cr[2] & 0x80000000u)) {
        u32 cuapr = mem_r32(0xFFF7F000u + CMMU_UAPR);
        u32 duapr = mem_r32(0xFFF7E000u + CMMU_UAPR);
        u32 pa1000 = 0; (void)mmu_walk(cuapr, 0x1000u, &pa1000);
        if (!quiet_uproc)
            printf("[ldctx-rte] PSR=%08x EPSR=%08x SXIP=%08x SNIP=%08x SFIP=%08x\n"
                   "            codeUAPR=%08x dataUAPR=%08x  user0x1000 -> pa=%08x word=%08x\n",
                   cpu.cr[1], cpu.cr[2], cpu.cr[4], cpu.cr[5], cpu.cr[6],
                   cuapr, duapr, pa1000, pa1000 ? mem_r32(pa1000) : 0);
    }

    /* Deliver a periodic hardclock interrupt once the kernel has enabled
       interrupts (PSR IND clear) and we are at an instruction boundary. */
    if (clock_irq && sysmode && !cpu.has_pending && !(cpu.cr[1] & 2u)
        && cpu.cr[7] >= 0xC0000000u && cpu.count >= next_clock) {
        next_clock = cpu.count + clock_period;
        irq_source = 0x40;                  /* hardclock (bit 27) */
        deliver_exception(1);               /* interrupt vector */
        return 0;
    }

    if (sysmode && force_sig_pc && pc == force_sig_pc)
        cpu.r[2] = force_sig_val;


    /* realmm: snoop the pmap mapper for device mappings (VA=r3 -> PA=r4) */
    if (realmm && pc == PMAP_MAP_FN && RD(4) >= 0xE0000000u)
        devmap_add(RD(3), RD(4));

    /* realmm: the kernel stores the TCS mailbox VA (r7) at _pmap_kernel+0xd74;
       track where it actually resolves so the handshake meets the kernel */
    if (realmm && pc == 0xC00A0E80u)
        tcs_mbox_pa = translate(RD(7), 0);

    /* Root mount: ufs_mountroot(&rootdev, "") is dispatched here (c0071dbc).
       rootdev is the SCSI disk; the mount reads its UFS superblock, which is
       all zeros on a blank disk.img, so the magic check fails -> "cannot mount
       root".  Booting needs a real UFS root (miniroot) on rootdev. */
    if (scsi_trace && sysmode && pc == 0xC0071DBCu) {
        printf("[mountroot] rootdev=%08x bootdev=%08x mountop@%08x @%llu\n",
               mem_r32(translate(0xC1015A48u, 0)),
               mem_r32(translate(0xC009EB40u, 0)),
               RD(9), (unsigned long long)cpu.count);
    }

    if (pc == 0xc0017498u) lctx_switch(RD(2));

    if (lct_trace && pc == 0xc0071db8u) {   /* vfs_mountroot's jsr to fs mountroot */
        printf("[mount] jsr arg=%08x curproc=%08x cur_u98=%08x r31=%08x @%llu\n",
               RD(2), mem_r32(translate(0xFBFFE0F0u, 0)), cur_u98, RD(31),
               (unsigned long long)cpu.count);
    }
    if (lct_trace && pc == 0xc004e954u) {   /* __gh_mvb: r1=procdup() return */
        printf("[fork] procdup ret r2=%08x cur_u98=%08x -> %s @%llu\n",
               RD(2), cur_u98, RD(2) ? "CHILD-path" : "parent-path",
               (unsigned long long)cpu.count);
    }
    if (lct_trace && pc == 0xc0055cc0u) {   /* _swtch_pri entry */
        u32 wq_lo = mem_r32(translate(0xC00145B0u, 0));
        u32 wq_hi = mem_r32(translate(0xC00145B4u, 0));
        printf("[swtch] pri=%08x caller=%08x curproc=%08x whichqs=%08x:%08x @%llu\n",
               RD(2), RD(1), mem_r32(translate(0xFBFFE0F0u, 0)), wq_hi, wq_lo,
               (unsigned long long)cpu.count);
    }

    if (watch_pc && pc == watch_pc) {
        printf("[watch] pc=%08x r1=%08x r2=%08x r3=%08x r4=%08x r5=%08x r6=%08x @%llu\n",
               pc, RD(1), RD(2), RD(3), RD(4), RD(5), RD(6),
               (unsigned long long)cpu.count);
    }

    u32 w   = mem_r32(translate(pc, 1));
    u32 op  = w >> 26;
    u32 D   = (w >> 21) & 31;
    u32 S1  = (w >> 16) & 31;
    u32 imm = w & 0xFFFF;
    u32 a   = RD(S1);

    u32 next_pc  = pc + 4;
    u32 branch   = 0;
    int taken    = 0;
    int delayed  = 0;

    cpu.count++;

    if (op < 0x10) {                        /* load/store/exchange, immediate */
        memop(op, D, a + imm);
    } else if (op < 0x20) {                 /* logical / arithmetic immediate */
        u32 b = imm;
        switch (op) {
        case 0x10: WR(D, a & (0xFFFF0000u | b)); break;      /* and   (lo half) */
        case 0x11: WR(D, a & (0x0000FFFFu | (b << 16))); break; /* and.u        */
        case 0x12: WR(D, a & b); break;                      /* mask            */
        case 0x13: WR(D, a & (b << 16)); break;              /* mask.u          */
        case 0x14: WR(D, a ^ b); break;
        case 0x15: WR(D, a ^ (b << 16)); break;
        case 0x16: WR(D, a | b); break;
        case 0x17: WR(D, a | (b << 16)); break;
        case 0x18: WR(D, a + b); break;                      /* addu            */
        case 0x19: WR(D, a - b); break;                      /* subu            */
        case 0x1A: if (!b) goto divzero; WR(D, a / b); break;
        case 0x1B: WR(D, a * b); break;
        case 0x1C: WR(D, (u32)((s32)a + (s32)b)); break;
        case 0x1D: WR(D, (u32)((s32)a - (s32)b)); break;
        case 0x1E: if (!b) goto divzero; WR(D, (u32)((s32)a / (s32)b)); break;
        case 0x1F: WR(D, do_cmp(a, b)); break;
        }
    } else if (op == 0x20) {                             /* control registers */
        u32 sub = (w >> 11) & 31, cr = (w >> 5) & 63;
        switch (sub) {
        case 0x08: case 0x09: WR(D, cpu.cr[cr]); break;            /* ldcr  */
        case 0x10: case 0x11: cpu.cr[cr] = a; break;               /* stcr  */
        case 0x18: case 0x19: { u32 o = cpu.cr[cr]; cpu.cr[cr] = a; WR(D, o); } break;
        default: goto badinsn;
        }
    } else if (op == 0x21) {                                          /* FPU */
        u32 fop = (w >> 11) & 0x1F;
        int td = (w >> 9) & 3, t1 = (w >> 7) & 3, t2 = (w >> 5) & 3;
        u32 S2 = w & 31;
        switch (fop) {
        case 0x05: fp_write(D, td, fp_read(S1,t1) + fp_read(S2,t2)); break; /* fadd */
        case 0x06: fp_write(D, td, fp_read(S1,t1) - fp_read(S2,t2)); break; /* fsub */
        case 0x00: fp_write(D, td, fp_read(S1,t1) * fp_read(S2,t2)); break; /* fmul */
        case 0x0E: fp_write(D, td, fp_read(S1,t1) / fp_read(S2,t2)); break; /* fdiv */
        case 0x0F: fp_write(D, td, sqrt(fp_read(S2,t2))); break;            /* fsqrt */
        case 0x04: fp_write(D, td, (double)(s32)RD(S2)); break;   /* flt int->fp */
        case 0x09:                                               /* int  (round) */
            WR(D, (u32)(s32)llrint(fp_read(S2,t2))); break;
        case 0x0A:                                               /* nint (round) */
            WR(D, (u32)(s32)llrint(fp_read(S2,t2))); break;
        case 0x0B:                                               /* trnc (toward 0) */
            WR(D, (u32)(s32)trunc(fp_read(S2,t2))); break;
        case 0x07: {                                             /* fcmp */
            double x = fp_read(S1,t1), y = fp_read(S2,t2);
            u32 r = 0;
            if (!(x < y || x > y || x == y)) r |= 0x1;   /* un: unordered */
            if (x == y) r |= 0x2;                        /* eq */
            if (x != y) r |= 0x4;                        /* ne */
            if (x > y)  r |= 0x8;                        /* gt */
            if (x <= y) r |= 0x10;                       /* le */
            if (x < y)  r |= 0x20;                       /* lt */
            if (x >= y) r |= 0x40;                        /* ge */
            WR(D, r); break;
        }
        default: goto badinsn;
        }
    } else if (op >= 0x30 && op <= 0x33) {          /* br / bsr, 26-bit disp */
        s32 off = (s32)(w << 6) >> 6;               /* sign-extend 26 bits   */
        branch = pc + (off << 2);
        taken = 1;
        delayed = op & 1;
        if (op >= 0x32) WR(1, delayed ? pc + 8 : pc + 4);          /* bsr    */
    } else if (op >= 0x34 && op <= 0x37) {                  /* bb0 / bb1     */
        s32 off = (s32)(int16_t)imm;
        u32 bit = (a >> D) & 1;
        int want = (op >= 0x36);
        delayed = op & 1;
        if (bit == (u32)want) { branch = pc + (off << 2); taken = 1; }
    } else if (op == 0x3A || op == 0x3B) {                        /* bcnd    */
        s32 off = (s32)(int16_t)imm;
        delayed = (op == 0x3B);
        if (cond_true(D, a)) { branch = pc + (off << 2); taken = 1; }
    } else if (op == 0x3C) {              /* bit-field immediate, and traps  */
        u32 sub = (w >> 10) & 63;
        if (sub == 0x34 || sub == 0x36 || sub == 0x3A) {           /* tb0/tb1/tcnd */
            int fire = (sub == 0x34) ? !((a >> D) & 1)
                     : (sub == 0x36) ?  ((a >> D) & 1)
                                     :  cond_true(D, a);
            if (fire) { trap_taken = 1; trap_vector = w & 0x1FF; trap_pc = pc; return 1; }
        } else {
            bitfield(sub, D, a, (w >> 5) & 31, w & 31);
        }
    } else if (op == 0x3D) {                              /* triadic register */
        u32 sub = (w >> 10) & 63;
        u32 var = (w >> 5) & 31;
        u32 S2  = w & 31;
        u32 b   = RD(S2);
        if (sub < 0x10) {                                 /* memory, register */
            u32 idx = (var & 0x10) ? b * memop_scale[sub] : b;
            memop(sub, D, a + idx);
        } else if (sub < 0x20) {                          /* logical / arith  */
            switch (sub) {
            case 0x10: WR(D, a & b); break;
            case 0x11: WR(D, a & ~b); break;                   /* and.c */
            case 0x14: WR(D, a ^ b); break;
            case 0x15: WR(D, a ^ ~b); break;                   /* xor.c */
            case 0x16: WR(D, a | b); break;
            case 0x17: WR(D, a | ~b); break;                   /* or.c  */
            case 0x18: WR(D, a + b); break;
            case 0x19: WR(D, a - b); break;
            case 0x1A: if (!b) goto divzero; WR(D, a / b); break;
            case 0x1B: WR(D, a * b); break;
            case 0x1C: WR(D, (u32)((s32)a + (s32)b)); break;
            case 0x1D: WR(D, (u32)((s32)a - (s32)b)); break;
            case 0x1E: if (!b) goto divzero; WR(D, (u32)((s32)a / (s32)b)); break;
            case 0x1F: WR(D, do_cmp(a, b)); break;
            default: goto badinsn;
            }
        } else if (sub >= 0x20 && sub <= 0x2A) {              /* bit-field    */
            bitfield(sub, D, a, (b >> 5) & 31, b & 31);
        } else if (sub == 0x3A) {          /* ff1: bit# of highest set bit    */
            WR(D, b ? (u32)(31 - __builtin_clz(b)) : 32u);
        } else if (sub == 0x3B) {          /* ff0: bit# of highest clear bit  */
            u32 nb = ~b;
            WR(D, nb ? (u32)(31 - __builtin_clz(nb)) : 32u);
        } else if (sub >= 0x30 && sub <= 0x33) {              /* jmp / jsr    */
            delayed = sub & 1;
            if (sub >= 0x32) WR(1, delayed ? pc + 8 : pc + 4);
            branch = b & ~3u;
            taken = 1;
        } else if (sub == 0x3F) {                             /* rte          */
            /* Return from exception: restore PSR from EPSR and resume at the
               first VALID pipeline register -- SXIP, then SNIP, then SFIP (the
               V bit is bit 1).  The kernel relies on this ordering: saveregs
               invalidates SNIP and points SFIP at a continuation to "call" it
               via rte, and interrupt entry invalidates SXIP so the real resume
               point is SNIP.  Process launch / fault return set SNIP valid. */
            cpu.cr[1] = cpu.cr[2];       /* restore PSR from EPSR */
            u32 r = (cpu.cr[4] & 2u) ? cpu.cr[4]
                  : (cpu.cr[5] & 2u) ? cpu.cr[5]
                                     : cpu.cr[6];
            branch = r & ~3u;
            taken = 1;
        } else {
            goto badinsn;
        }
    } else {
        goto badinsn;
    }

    if (realmm && dbg_trans && pc >= 0xC0000000u) {
        u32 npc = (cpu.has_pending) ? cpu.pending
                : taken ? (delayed ? next_pc : branch) : next_pc;
        if (npc < 0xC0000000u && npc >= 0x8000u && npc < 0xE0000000u)
            printf("[VA->phys] %08x (%s) -> %08x\n", pc,
                   taken ? "branch" : "fall", npc);
    }

    /* --- sequencing: delay slots are explicit --- */
    if (cpu.has_pending) {
        cpu.has_pending = 0;
        cpu.pc = cpu.pending;
    } else if (taken) {
        if (delayed) { cpu.pending = branch; cpu.has_pending = 1; cpu.pc = next_pc; }
        else cpu.pc = branch;
    } else {
        cpu.pc = next_pc;
    }
    return 0;

divzero:
    trap_taken = 1; trap_vector = 6; trap_pc = pc; return 1;
badinsn:
    trap_taken = 1; trap_vector = (u32)-1; trap_pc = pc; return 1;
}
