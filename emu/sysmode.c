/* Generated split of the former single-file nx88.c.
   Shared types, macros, hot inline accessors, and all
   cross-module declarations live in nx88.h. */
#include "nx88.h"

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
            if (utest) { launch_utest(); continue; }   /* drop to user mode */
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
            char m[160];
            mem_cstr(RD(2), m, sizeof m);
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
                if (fd_watch_pipe) fd_pipe[RD(2)] = fd_watch_pipe;
                if (fd_watch_disk) { fd_disk[RD(2)] = 1; disk_off[RD(2)] = 0; }
            }
            if (fd_watch_pair && RD(3) < 64 && RD(2) < 64) {
                fd_kernel[RD(3)] = 1;                  /* pipe(2): two fds */
                int pi = pipe_alloc();                 /* ...one buffer     */
                if (pi >= 0) fd_pipe[RD(2)] = fd_pipe[RD(3)] = (u8)(pi + 1);
            }
            fd_watch_pc = 0; fd_watch_pair = 0; fd_watch_con = 0; fd_watch_disk = 0;
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
                if (trace_traps)
                    printf("[trap] pid %d vec %u pc=%08x  syscall r9=%-4u "
                           "args %08x %08x %08x @%llu\n",
                           ucur >= 0 ? uprocs[ucur].pid : 0,
                           trap_vector, trap_pc, RD(9), RD(2), RD(3), RD(4),
                           (unsigned long long)cpu.count);
                /* stdin/stdout/stderr never reach the kernel (see the console
                   notes above); everything else goes to the real handlers. */
                if (trap_vector == 128 && !(cpu.cr[1] & 0x80000000u)
                    && console_syscall(RD(9), trap_pc)) {
                    trap_taken = 0;
                    continue;
                }
                /* Raw sd0 read/write/lseek serviced from disk.img (see
                   disk_syscall) -- the kernel's raw DMA can't reach our
                   synthetic user buffers. */
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
                /* fork/execve/wait/exit are serviced here too; see the
                   process-management notes above. */
                if (trap_vector == 128 && !(cpu.cr[1] & 0x80000000u)
                    && uproc_syscall(RD(9), trap_pc)) {
                    trap_taken = 0;
                    if (uproc_all_done) {
                        if (interactive)
                            printf("\n[halt] shell exited -- machine halted.\n");
                        else
                            printf("[uproc] all processes exited @%llu\n",
                                   (unsigned long long)cpu.count);
                        break;
                    }
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
                    }
                    /* dup: whatever the new descriptor turns out to be, it is
                       the host's terminal exactly when the source was.  The
                       shell reads its input through a dup of fd 0, so without
                       this its `read` builtin sees EOF. */
                    fd_watch_con = (RD(9) == 41 && RD(2) < 64) ? fd_console[RD(2)] : 0;
                    fd_watch_pipe = (RD(9) == 41 && RD(2) < 64) ? fd_pipe[RD(2)] : 0;
                    if (RD(9) == 41 && RD(2) < 64 && fd_disk[RD(2)]) fd_watch_disk = 1;
                }
                if (trap_vector == 128 && !(cpu.cr[1] & 0x80000000u) && RD(9) == 6
                    && RD(2) < 64)
                    fd_kernel[RD(2)] = fd_console[RD(2)] = fd_pipe[RD(2)] =
                        fd_disk[RD(2)] = 0;
                /* dup2 replaces the target outright, terminal-ness included --
                   this is how a shell redirect takes stdout off the console. */
                if (trap_vector == 128 && !(cpu.cr[1] & 0x80000000u)
                    && RD(9) == 90 && RD(3) < 64 && RD(2) < 64)
                    fd_console[RD(3)] = fd_console[RD(2)],
                    fd_pipe[RD(3)] = fd_pipe[RD(2)],
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
