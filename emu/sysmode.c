/* Generated split of the former single-file nx88.c.
   Shared types, macros, hot inline accessors, and all
   cross-module declarations live in nx88.h. */
#include "nx88.h"

/* Snapshot the kernel's process table: who exists, what state they are in, and
   what they are asleep on.  Printed with -v when the run stops, which is the
   only way to see WHY a run went idle -- the machine sitting in swtch tells you
   nothing by itself. */
void proc_table_dump(void)
{
    u32 pb = mem_r32(translate(0xC1015758u, 0));
    u32 np = mem_r32(translate(0xC1014B80u, 0));
    printf("  proc table (stat: 1=SLEEP 3=RUN 4=IDL 5=ZOMB):\n");
    for (u32 i = 0, shown = 0; pb && i < np && shown < 16; i++) {
        u32 q = pb + i * 512u, st = mem_r32(translate(q + 0x40u, 0));
        if (!st) continue;
        shown++;
        printf("    proc %08x pid %-5u stat %u  wchan %08x  link %08x rlink %08x"
               "  runq %08x  parent %08x  clu %u\n",
               q, mem_r32(translate(q + 0x4Cu, 0)) >> 16, st,
               mem_r32(translate(q + 0x44u, 0)),
               mem_r32(translate(q, 0)), mem_r32(translate(q + 4u, 0)),
               mem_r32(translate(q + 0x80u, 0)),
               mem_r32(translate(q + 0x18u, 0)),
               mem_r32(translate(q + 0x90u, 0)));
    }
}

int run_sys(const char *path, u64 limit, u32 sig)
{
    AOut a;
    if (aout_load(path, &a)) return 1;
    sysmode = 1;
    force_sig_pc  = sig ? 0xC00A851Cu : 0;
    force_sig_val = sig;

    /* realmm loads at true physical addresses (VA - KOFF); otherwise identity */
    u32 tload = realmm ? TEXT_BASE - KOFF : TEXT_BASE;
    u32 dload = realmm ? DATA_BASE - KOFF : DATA_BASE;
    /* --dataphys: nX links its data at VA 0xC1000000 but expects it loaded
       PHYSICALLY right after text.  Its own numbers say so: the kernel map's
       page tables live at VA 0xC103E000 and its APR names PA 0x00110000 -- a
       difference of exactly DATA_BASE - (tload + text), and 0x110000 is the
       first page past data+bss under that layout, i.e. where its early physical
       allocator starts.  With data at the wrong PA the kernel's tables are
       unreachable physically and every walk falls back to the synthetic map. */
    if (realmm && dataphys) {
        dload = tload + a.text;
        kdata_va = DATA_BASE;
        kdata_off = DATA_BASE - dload;
    }
    mem_load(tload, a.img + HDR_PAGE, a.text);
    mem_load(dload, a.img + HDR_PAGE + a.text, a.data);
    mem_zero(dload + a.data, a.bss);

    printf("kernel: text %u @ phys %08x  data %u @ phys %08x  bss %u  entry %08x\n",
           a.text, tload, a.data, dload, a.bss, a.entry);
    if (sig) printf("forcing TCS EEPROM signature '%c' at %08x\n",
                    (char)sig, force_sig_pc);

    if (synth_boot) { boot_build_tables(); build_free_list(); }

    /* Node-presence bitmap.  bfly_init scans slots 0..511 of the node-config
       device at 0xFE00191C (one byte each); a byte in 0x00-0x7F reads "present"
       (cmp/bb1-ge), 0x80-0xFF "absent", and it counts only present slots into
       the node count at [0xC1014D40].  An unmapped device reads 0 -> all 512
       look present -> the kernel believes it has 64 nodes and later times out
       synchronizing 63 phantom RTCs (panic: synchrtc).  Mark only slots
       [0, cfg_nodes) present so the machine is genuinely single-node.  Opt-in
       via --nodes=N: making the machine truly 1-node makes the scheduler run
       (and context-switch) far earlier, which then trips the idle-proc _sleep
       assertion at ~3.7M -- so the default leaves the 64-node path intact and
       skips synchrtc instead (see the synchrtc intercept). */
    if (cfg_nodes)
        for (u32 i = cfg_nodes; i < 512; i++)
            mem_w8(0xFE00191Cu + i, 0xFF);

    memset(&cpu, 0, sizeof cpu);
    cpu.pc = a.entry;
    WR(31, DATA_BASE + a.data + a.bss + 0x8000);
    /* Reset PSR: supervisor + interrupts disabled (IND) + shadow frozen, like
       the MC88100 at reset.  The kernel clears IND once its vector table and
       interrupt handlers are up, which is when clock delivery may begin. */
    cpu.cr[1] = 0x80000003u;

    clock_t t0 = clock();
    u32 last_kpc = cpu.pc;
    /* PC ring buffer: the last PCH_N program counters, for post-mortem tracing
       of the crash/halt sequence.  Cheap enough to keep always on. */
    #define PCH_N 65536
    static u32 pchist[PCH_N]; static unsigned pchpos = 0;
    while (cpu.count < limit) {
        pchist[pchpos++ & (PCH_N - 1)] = cpu.pc;
        /* derail catcher: kernel boot stays in kernel space (>=0xC0000000);
           a fetch below it means a bad jump -- report the last kernel pc.
           Exempt genuine user-mode execution (PSR bit31 clear), where a low PC
           is expected. */
        if (realmm && cpu.pc < 0xC0000000u && (cpu.cr[1] & 0x80000000u)) {
            printf("[derail] jumped to %08x from kernel pc=%08x @%llu\n",
                   cpu.pc, last_kpc, (unsigned long long)cpu.count);
            printf("[derail] last sleep: chan=%08x from=%08x\n",
                   last_sleep_chan, last_sleep_from);
            if (dump_pchist) {
                printf("--- last %d PCs before the derail ---\n", PCH_N);
                for (unsigned k = 0; k < PCH_N; k++)
                    printf("%08x%s", pchist[(pchpos + k) & (PCH_N - 1)],
                           (k % 8 == 7) ? "\n" : " ");
                printf("\n");
            }
            break;
        }
        /* BOOT COMPLETE: the swapper's sched() idle loop is `for(;;)
           sleep(&proc0)` -- _sleep entry c0054720 called from c004859c with
           chan &proc0 (=c0047f88).  Reaching it means main() finished: root is
           mounted, kernel/daemon threads are created, and proc0 goes idle.
           (proc0 can't actually sleep -- it's still on the run queue -- so
           without this catcher it would crash into doadump; stop cleanly here
           and report the milestone instead.) */
        if (cpu.pc == 0xC0054720u && RD(1) == 0xC004859Cu && RD(2) == 0xC0047F88u) {
            if (kmsgs) kmsg_flush();
            printf("[boot-complete] kernel reached the swapper sched() idle "
                   "loop @%llu -- main() done, root mounted.\n",
                   (unsigned long long)cpu.count);
            if (procexp) { if (proc_experiment()) continue; break; }  /* real proc */
            if (vmexp) { vm_experiment(); break; }      /* step-2 VM RPC probe */
            break;
        }
        /* The process has exited and the kernel switched back to
           proc0, which we parked on a br-to-self.  Report and stop. */
        if (procexp && cpu.pc == PROCEXP_IDLE) {
            /* CPU 0 has nothing to run.  Ask the KERNEL's own scheduler for
               the next process -- there is no hand-dispatch here any more.
               proc0 is parked on a br-to-self so it never yields and the
               scheduler never gets a turn; that is the only reason the emulator
               ever had to pick a process itself (it used to scan the proc table
               for a runnable descendant and load_context it by hand, standing
               in for the 63 CPUs this model does not run).  Per-process u-areas
               made the kernel's scheduler work properly and retired it. */
            /* Anything of ours still in play -- running, runnable or asleep --
               but not a zombie (nobody reaps ours; proc0 is their parent). */
            u32 pb = mem_r32(translate(0xC1015758u, 0));
            u32 np = mem_r32(translate(0xC1014B80u, 0));
            int alive = 0;
            for (u32 i = 0; pb && i < np && !alive; i++) {
                u32 q = pb + i * 512u, st = mem_r32(translate(q + 0x40u, 0));
                if (st == 0 || st == 5) continue;
                u32 a2 = q;
                for (int hop = 0; a2 && a2 != procexp_proc && hop < 32; hop++)
                    a2 = mem_r32(translate(a2 + 0x18u, 0));
                if (a2 == procexp_proc) alive = 1;
            }
            /* ★★ Let the KERNEL's own scheduler do the
               dispatching -- no hand-dispatch at all.  proc0 is parked on a
               br-to-self here, so it never yields and the scheduler never gets
               a turn; that is the ONLY reason this hook ever had to pick a
               process itself.  Jump into swtch_pri(-1) (c0055cc0) with r1
               pointing back at the sentinel: it picks the highest-priority
               runnable proc, sets both curprocs and load_contexts it, and the
               machine finds its own way back here.

               swtch_pri switches away WITHOUT queueing the outgoing proc (its
               normal callers -- sleep and friends -- have already parked
               themselves somewhere), so queue proc0 first or the scheduler can
               never come back to us.

               `alive` rather than `pick` decides when to stop: a descendant
               that is merely ASLEEP (the shell in wait4 while its child runs)
               still counts, or we would declare victory mid-script.  Bail out
               if two yields in a row achieve nothing -- that is a real
               deadlock, not a stop condition. */
            if (alive) {
                static u64 last_count; static int idle_spins;
                idle_spins = (cpu.count - last_count < 2000ull) ? idle_spins + 1 : 0;
                last_count = cpu.count;
                if (idle_spins < 2) {
                    { u32 p0 = mem_r32(translate(0xC0014044u, 0));
                      /* Queue proc0 at the WORST priority (p+0x84 is the user
                         priority, +0x85 the current one; the queue index is
                         pri/4, so 127 -> list 31).  Left at the swapper's own
                         priority it is the best thing on the queue, swtch_pri
                         picks it, sees it is already current, and returns
                         without switching -- the yield achieves nothing.
                         Lowest priority is what an idle loop should have. */
                      if (p0) { mem_w8(translate(p0 + 0x84u, 0), 127);
                                mem_w8(translate(p0 + 0x85u, 0), 127); }
                      runq_add(p0); }
                    WR(1, PROCEXP_IDLE);           /* return here afterwards */
                    WR(2, 0xFFFFFFFFu);            /* swtch_pri(-1): any proc */
                    cpu.pc = 0xC0055CC0u;
                    continue;
                }
                printf("[procexp] scheduler made no progress -- deadlock?\n");
            }
            /* ★ WARN ABOUT UNFLUSHED DISK WRITES.  This harness ends the
               moment our process tree exits -- there is no shutdown, so the
               kernel's buffer cache can still hold dirty FFS metadata
               (cylinder-group block and inode bitmaps, the superblock summary,
               inodes).  Those never reach disk.img, and the NEXT run mounts a
               filesystem whose bitmaps say "free" for blocks and inodes the
               directory tree is really using.  The next allocation collides:
               `ialloc: dup alloc' or `free: freeing free block' -- a panic in a
               later run caused by writes silently lost in an earlier one.

               We cannot flush it ourselves: sync() has to sleep waiting for its
               I/O, and calling it from here (proc0 parked on the idle sentinel,
               which never yields to the scheduler) just runs away -- measured,
               30M instructions and no return.  It has to run as a real process,
               so the guest has to ask for it.  Say so loudly instead. */
            if (disk_wrote && !fs_synced)
                printf("[disk] ★ WARNING: disk.img was written through the "
                       "buffer cache but the guest never ran sync or umount, so "
                       "dirty\n       filesystem metadata did NOT reach the "
                       "image.  A LATER run will panic `ialloc: dup alloc' or\n"
                       "       `free: freeing free block'.  End the script with "
                       "`/etc/umount /mnt' (or `sync').\n");
            if (interactive)
                printf("\n[halt] shell exited -- machine halted.\n");
            else
                printf("[procexp] all our processes exited @%llu\n",
                       (unsigned long long)cpu.count);
            if (!verbose_sys) break;
            proc_table_dump();
            if (1) break;
            for (u32 i = 0, shown = 0; pb && i < np && shown < 12; i++) {
                u32 q = pb + i * 512u, st = mem_r32(translate(q + 0x40u, 0));
                if (!st) continue;
                shown++;
                printf("    proc %08x #%-4u pid %-5u stat %u node %u clu %u\n",
                       q, i, mem_r32(translate(q + 0x4Cu, 0)) >> 16, st,
                       mem_r32(translate(q + 0x7Cu, 0)),
                       mem_r32(translate(q + 0x90u, 0)));
            }
            break;
        }
        /* halt catcher: _tcs_shutdown+0x38 and _doadump+0x20 are br-to-self
           spins reached only after a panic/reboot has run its course */
        if (cpu.pc == 0xC00A24F4u || cpu.pc == 0xC00A24B8u) {
            printf("[halt] reached %s spin at %08x @%llu\n",
                   cpu.pc == 0xC00A24F4u ? "tcs_shutdown" : "doadump",
                   cpu.pc, (unsigned long long)cpu.count);
            if (dump_pchist) {
                printf("--- last %d PCs before halt ---\n", PCH_N);
                for (unsigned k = 0; k < PCH_N; k++)
                    printf("%08x%s", pchist[(pchpos + k) & (PCH_N - 1)],
                           (k % 8 == 7) ? "\n" : " ");
                printf("\n");
            }
            break;
        }
        /* Remember the last thing the kernel tried to sleep on; the panic
           report below is far more useful with the channel in hand. */
        if (cpu.pc == 0xC0054A64u) {
            last_sleep_chan = RD(2);
            last_sleep_from = RD(1);
        }
        /* _panic: the message is the single most useful thing the kernel can
           tell us, and it is lost if we only catch the throttled printf. */
        if (cpu.pc == 0xC005AFE0u) {
            /* The message pointer is a kernel VA -- mem_cstr takes a PHYSICAL
               address, so under --dataphys (where kernel VA != PA) it silently
               produced an empty string and every panic printed blank.  Read it
               through translate(). */
            char m[160];
            uread_str(RD(2), m, sizeof m);
            printf("[PANIC] %s  (from %08x, @%llu)\n", m, RD(1),
                   (unsigned long long)cpu.count);
            printf("[PANIC] last sleep: chan=%08x from=%08x\n",
                   last_sleep_chan, last_sleep_from);
            if (dump_pchist) {
                printf("--- last %d PCs before the panic ---\n", PCH_N);
                for (unsigned k = 0; k < PCH_N; k++)
                    printf("%08x%s", pchist[(pchpos + k) & (PCH_N - 1)],
                           (k % 8 == 7) ? "\n" : " ");
                printf("\n");
            }
        }
        /* _simple_lock_failed / _pmap_lock: report who spun on what, once. */
        if (cpu.pc == 0xC0018170u || cpu.pc == 0xC00A7C78u) {
            static int once;
            if (!once && dump_pchist) {
                printf("--- last %d PCs before the spin ---\n", PCH_N);
                for (unsigned k = 0; k < PCH_N; k++)
                    printf("%08x%s", pchist[(pchpos + k) & (PCH_N - 1)],
                           (k % 8 == 7) ? "\n" : " ");
                printf("\n");
            }
            if (!once++)
                printf("[lockspin] simple_lock(%08x) failed; r1=%08x r26=%08x "
                       "r25=%08x r27=%08x @%llu\n", RD(2), RD(1), RD(26),
                       RD(25), RD(27), (unsigned long long)cpu.count);
        }
        if (trace_pc_until && cpu.count < trace_pc_until)
            printf("[pctrace] %08x\n", cpu.pc);
        last_kpc = cpu.pc;
        /* Kernel console output.  subr_prf.o funnels every character printf
           produces through _putchar(c, ...), whatever the message started out
           as -- so hooking it prints the kernel's own log, fully formatted,
           instead of reconstructing it from format strings.  The real putchar
           goes on to msgbuf and cnputc; we only watch. */
        if (kmsgs && cpu.pc == KERN_PUTCHAR)
            kmsg_putchar((int)(RD(2) & 0xff), RD(3));
        if (brk_watch_pc && !(cpu.cr[1] & 0x80000000u)
            && (cpu.pc == brk_watch_pc + 4u || cpu.pc == brk_watch_pc + 8u)) {
            if (!quiet_uproc)
                printf("[brk] obreak(%08x) -> %s, r2=%08x r3=%08x\n", brk_watch_arg,
                       cpu.pc == brk_watch_pc + 8u ? "ok" : "ERR", RD(2), RD(3));
            brk_watch_pc = 0;
        }
        if (fd_watch_pc && cpu.pc == fd_watch_pc + 8u
            && !(cpu.cr[1] & 0x80000000u)) {
            if (RD(2) < 64) {
                fd_kernel[RD(2)] = 1;                  /* kernel owns this fd */
                if (fd_watch_con) fd_console[RD(2)] = 1;
                if (fd_watch_disk) { fd_disk[RD(2)] = 1; disk_off[RD(2)] = 0; }
            }
            if (fd_watch_pair && RD(3) < 64 && RD(2) < 64)
                fd_kernel[RD(3)] = 1;      /* pipe(2) returns TWO descriptors */
            fd_watch_pc = 0; fd_watch_pair = 0; fd_watch_con = 0; fd_watch_disk = 0;
        }
        if (ufault_pending) {
            /* A real process touched a page its exec mapped demand-paged.
               Hand it to the kernel's own MC88100 handler as a genuine access
               fault -- vector 2 for an instruction fetch, 3 for data -- and let
               its vm_fault resolve it.  This is the only route that can do
               copy-on-write or vnode paging.

               There used to be a fallback here (--no-hwfault) that resolved
               the fault itself with vm_map_pageable over the faulting 8K.  It
               could never break copy-on-write -- vm_map_pageable(wire) reports
               success on a COW entry without materialising the page -- so it
               could not run a forked shell, and it is gone. */
            ufault_pending = 0;
            if (!interactive && ufaults++ < (verbose_sys ? 100000u : 12u))
                printf("[hwfault] pid %d %08x (%s%s) pc=%08x %s @%llu\n",
                       real_pid(), ufault_va,
                       ufault_code ? "code" : "data",
                       ufault_code ? "" : (ufault_write ? " write" : " read"),
                       ufault_pc,
                       (cpu.cr[1] & 0x80000000u) ? "SUPERVISOR" : "user",
                       (unsigned long long)cpu.count);
            deliver_fault(ufault_code ? 2u : 3u, cpu.pc, ufault_va,
                          ufault_code, ufault_write, ufault_width);
            continue;
        }
        if (step()) {
            /* Faithful userland: deliver real synchronous traps (syscalls via
               vector 128, page faults via vector 2, etc.) to the kernel's own
               handlers instead of stopping, so user code runs under nX.  A
               vector of -1 is an unimplemented instruction (an emulator gap,
               not a real trap) -- keep stopping on that so bugs stay visible.
               The boot takes zero traps, so this never fires before userland;
               gated on --deliver-traps to keep the default boot untouched. */
            if (deliver_traps && trap_vector != (u32)-1
                && cpu.cr[7] >= 0xC0000000u) {
                /* Swap in the current process's descriptor flags before any
                   of the fd-aware intercepts look at them. */
                if (trap_vector == 128 && !(cpu.cr[1] & 0x80000000u))
                    fd_switch(real_pid(), real_ppid());
                /* nX sysent: 34 = sync, 159 = unmount (both flush the cache) */
                if (trap_vector == 128 && !(cpu.cr[1] & 0x80000000u)
                    && (RD(9) == 34 || RD(9) == 159))
                    fs_synced = 1;
                if (trace_traps)
                    printf("[trap] pid %d vec %u pc=%08x  syscall r9=%-4u "
                           "args %08x %08x %08x @%llu\n",
                           real_pid(), trap_vector, trap_pc, RD(9),
                           RD(2), RD(3), RD(4),
                           (unsigned long long)cpu.count);
                /* stdin/stdout/stderr never reach the kernel (see the console
                   notes above); everything else goes to the real handlers. */
                if (trap_vector == 128 && !(cpu.cr[1] & 0x80000000u)
                    && console_syscall(RD(9), trap_pc)) {
                    trap_taken = 0;
                    continue;
                }
                /* Raw sd0 read/write/lseek serviced from disk.img (see
                   disk_syscall) -- the kernel's raw DMA does not land where the
                   faulting process reads it. */
                if (trap_vector == 128 && !(cpu.cr[1] & 0x80000000u)
                    && disk_syscall(RD(9), trap_pc)) {
                    trap_taken = 0;
                    continue;
                }
                /* Host-file passthrough (/hosttar, --hostfile): open/read/lseek/
                   close served from the host archive so guest tar can read it. */
                if (trap_vector == 128 && !(cpu.cr[1] & 0x80000000u)
                    && hostfile_syscall(RD(9), trap_pc)) {
                    trap_taken = 0;
                    continue;
                }
                /* Remember fd-returning syscalls so their result can be marked
                   kernel-owned when control comes back to user mode. */
                if (trap_vector == 128 && !(cpu.cr[1] & 0x80000000u)
                    && (RD(9) == 5 || RD(9) == 8 || RD(9) == 41 || RD(9) == 90
                        || RD(9) == 42))
                {
                    fd_watch_pc = trap_pc;
                    fd_watch_pair = (RD(9) == 42);
                    /* open()/creat() of a /dev sd0 device: the SCSI disk -- mark
                       the returned fd so its read/write/lseek/ioctl route to
                       disk.img.  mkfs opens the disk for writing via creat(8),
                       not open(5), so both must check the path (arg 0). */
                    fd_watch_disk = 0;
                    if (RD(9) == 5 || RD(9) == 8) {
                        char p[64];
                        uread_str(RD(2), p, sizeof p);
                        if (strstr(p, "sd0")) fd_watch_disk = 1;
                        if (verbose_sys)
                            printf("[open] pid %d \"%s\" @%llu\n", real_pid(), p,
                                   (unsigned long long)cpu.count);
                    }
                    /* dup: whatever the new descriptor turns out to be, it is
                       the host's terminal exactly when the source was.  The
                       shell reads its input through a dup of fd 0, so without
                       this its `read` builtin sees EOF. */
                    fd_watch_con = (RD(9) == 41 && RD(2) < 64) ? fd_console[RD(2)] : 0;
                    if (RD(9) == 41 && RD(2) < 64 && fd_disk[RD(2)]) fd_watch_disk = 1;
                }
                if (verbose_sys && trap_vector == 128
                    && !(cpu.cr[1] & 0x80000000u) && RD(9) == 59) {
                    char p[128];
                    uread_str(RD(2), p, sizeof p);
                    printf("[exec] pid %d \"%s\" @%llu\n", real_pid(), p,
                           (unsigned long long)cpu.count);
                }
                if (trap_vector == 128 && !(cpu.cr[1] & 0x80000000u) && RD(9) == 6
                    && RD(2) < 64)
                    fd_kernel[RD(2)] = fd_console[RD(2)] =
                        fd_disk[RD(2)] = 0;
                /* dup2 replaces the target outright, terminal-ness included --
                   this is how a shell redirect takes stdout off the console. */
                if (trap_vector == 128 && !(cpu.cr[1] & 0x80000000u)
                    && RD(9) == 90 && RD(3) < 64 && RD(2) < 64)
                    fd_console[RD(3)] = fd_console[RD(2)],
                    fd_disk[RD(3)] = fd_disk[RD(2)],
                    disk_off[RD(3)] = disk_off[RD(2)];
                deliver_trap(trap_vector, trap_pc);
                if (trace_traps) trace_pc_until = cpu.count + trace_len;
                trap_taken = 0;
                continue;
            }
            printf("trap %d at pc=%08x after %llu instructions\n",
                   (int)trap_vector, trap_pc, (unsigned long long)cpu.count);
            break;
        }
    }
    /* Hit the instruction limit without derailing/panicking -- usually a spin.
       Dump the ring so the loop body is visible (nothing else caught it). */
    if (verbose_sys && cpu.count >= limit) proc_table_dump();
    if (dump_pchist && cpu.count >= limit) {
        printf("--- last %d PCs at the instruction limit (spin?) ---\n", PCH_N);
        for (unsigned k = 0; k < PCH_N; k++)
            printf("%08x%s", pchist[(pchpos + k) & (PCH_N - 1)],
                   (k % 8 == 7) ? "\n" : " ");
        printf("\n");
    }
    if (!quiet_uproc)
        for (int k = 0; k < 32; k += 4) {
            printf("  ");
            for (int j = k; j < k + 4; j++) printf("r%-2d=%08x  ", j, RD(j));
            putchar('\n');
        }
    if (vmprobe) vm_probe_report();
    if (ctxtrace) ctx_report();
    if (ufaults) printf("user page faults resolved through the kernel's VM: %llu\n",
                        (unsigned long long)ufaults);
    if (ktab_bias) printf("user-space walks through the kernel's real tables: %llu\n",
                          (unsigned long long)kwalk_user);
    double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
    if (!quiet_uproc) {
        printf("tcs commands: %llu\n", (unsigned long long)tcs_commands);
        if (translate_on)
            printf("translation: %llu faults (last va %08x from pc %08x)\n",
                   (unsigned long long)xlat_faults, last_fault_va, last_fault_pc);
        printf("cmmu probes: %llu hit, %llu miss\n",
               (unsigned long long)probe_hits, (unsigned long long)probe_misses);
    }
    printf("stopped at pc=%08x after %llu instructions (%.2fs, %.1f Minsn/s)\n",
           cpu.pc, (unsigned long long)cpu.count, secs,
           secs > 0 ? cpu.count / secs / 1e6 : 0.0);
    return 0;
}
