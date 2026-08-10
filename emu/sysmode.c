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
            if (utest) { launch_utest(); continue; }   /* drop to user mode */
            break;
        }
        /* --procexp: the process has exited and the kernel switched back to
           proc0, which we parked on a br-to-self.  Report and stop. */
        if (procexp && cpu.pc == PROCEXP_IDLE) {
            /* CPU 0 has nothing to run.  The kernel believes it has 64
               processors, so a fork inside our logical cluster queues the child
               on ANOTHER node's run queue -- and that node's CPU never executes
               here.  Stand in for it: dispatch any still-runnable proc from our
               own cluster, whatever node it was queued on.  Only procs in our
               cluster are eligible, which excludes the boot's 64 idle threads
               (all cluster 0). */
            u32 pb = mem_r32(translate(0xC1015758u, 0));
            u32 np = mem_r32(translate(0xC1014B80u, 0));
            u32 pick = 0, pickpid = 0;
            for (u32 i = 0; procexp_cluster && pb && i < np; i++) {
                u32 q = pb + i * 512u;
                if (mem_r32(translate(q + 0x40u, 0)) != 3u ||
                    mem_r32(translate(q + 0x90u, 0)) != procexp_cluster)
                    continue;
                /* Highest pid first: that is the newest process, i.e. the child
                   a fork just made, rather than init sitting runnable since
                   boot on a node whose CPU never runs. */
                u32 pid = mem_r32(translate(q + 0x4Cu, 0)) >> 16;
                if (pid < procexp_pid) continue;   /* ours and its children only */
                if (pid >= pickpid) { pickpid = pid; pick = q; }
            }
            if (pick) {
                u32 ctx = mem_r32(translate(pick + 0xC8u, 0));
                printf("[procexp] dispatching proc %08x (pid %u, node %u) "
                       "@%llu\n", pick,
                       mem_r32(translate(pick + 0x4Cu, 0)) >> 16,
                       mem_r32(translate(pick + 0x7Cu, 0)),
                       (unsigned long long)cpu.count);
                /* Re-home it to node 0.  fork inside a multi-node cluster puts
                   the child on another node, and the kernel checks that a proc
                   runs on its home node ("acallpsig not on home node",
                   c00aba7c: proc+0x7c vs the CPU's node at [0xC0014008]).  We
                   only ever execute node 0's CPU, so make node 0 its home; the
                   memory it was given on the other node is still just memory. */
                mem_w32(translate(pick + 0x7Cu, 0), 0);
                mem_w32(translate(ctx + 0xB4u, 0), 0);
                runq_remove(pick);                          /* the scheduler's dequeue */
                set_curproc(pick, ctx);                      /* curproc, both copies */
                WR(2, ctx);
                cpu.pc = 0xC0017498u;                       /* load_context */
                continue;
            }
            printf("[procexp] all our processes exited @%llu\n",
                   (unsigned long long)cpu.count);
            if (!verbose_sys) break;
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
        if (ufault_pending) {
            /* A real process touched a page its exec mapped demand-paged.
               --hwfault takes the faithful route below (vector 2/3 to the
               kernel's own handlers).  Without it, resolve the fault the way
               the kernel would and re-execute: vm_map_pageable over the
               faulting page on the current process's own map.  That cannot do
               copy-on-write -- vm_map_pageable(wire) reports success on a COW
               entry without materialising the page -- which is what --hwfault
               exists to fix. */
            /* This kernel's VM granularity is 8K everywhere (vm_allocate,
               vm_map_u_area_create, and pmap_enter's descriptor pairs), so
               resolve a whole 8K block -- asking it to wire a 4K sub-range
               splits entries and leaves the text page zero-filled. */
            u32 va = ufault_va & ~0x1FFFu;
            u32 cp = mem_r32(translate(0xFBFFE0F0u, 0));
            u32 map = cp ? mem_r32(translate(cp + 0xD4u, 0)) : 0;
            ufault_pending = 0;
            /* --hwfault: hand it to the kernel's own handler instead of
               resolving it ourselves.  This is the faithful path and the only
               one that can do copy-on-write (a forked child's text) or vnode
               paging -- vm_map_pageable reports success on a COW entry without
               materialising the page. */
            if (hwfault) {
                if (ufaults++ < 12)
                    printf("[hwfault] %08x (%s%s) pc=%08x %s @%llu\n", ufault_va,
                           ufault_code ? "code" : "data",
                           ufault_code ? "" : (ufault_write ? " write" : " read"),
                           ufault_pc,
                           (cpu.cr[1] & 0x80000000u) ? "SUPERVISOR" : "user",
                           (unsigned long long)cpu.count);
                deliver_fault(ufault_code ? 2u : 3u, cpu.pc, ufault_va,
                              ufault_code, ufault_write, ufault_width);
                continue;
            }
            /* Run the kernel functions below on a KERNEL stack.  A fault taken
               in user mode leaves r31 pointing at the USER stack, and kcall
               does not change it -- so vm_map_pageable's own prologue pushed
               onto user memory, corrupting it, and eventually faulted itself
               (pc=c0092354, its `st r1,[r31+0x34]`).  cr17/SR1 is the kernel
               stack the trap path would have switched to; use that, and be in
               supervisor mode while doing it. */
            u32 sp0 = RD(31), psr0 = cpu.cr[1];
            if (!(psr0 & 0x80000000u)) {
                WR(31, cpu.cr[17]);
                cpu.cr[1] = psr0 | 0x80000000u;
            }
            int ok = map && !kcall(0xC0092350u, map, va, va + 0x2000u, 1, 0);
            if (!ok && map) {
                /* Not in the map at all -- a stack that has to grow, which the
                   kernel's own fault path would do for us.  Extend it with the
                   kernel's vm_allocate at that exact page, then wire it.
                   proc+0xe0 is a zero, unused word to hand vm_allocate as its
                   in/out address. */
                u32 slot = cp + 0xE0u;
                mem_w32(translate(slot, 0), va);
                ok = !kcall(0xC008EB6Cu, map, slot, 0x2000u, 0x90, 0xFFFFFFFFu)
                  && !kcall(0xC0092350u, map, va, va + 0x2000u, 1, 0);
            }
            WR(31, sp0);                        /* back to the faulting context */
            cpu.cr[1] = psr0;
            if (!ok) {
                printf("[ufault] cannot page in %08x (%s) from pc=%08x for "
                       "proc %08x @%llu\n", ufault_va,
                       ufault_code ? "code" : "data", ufault_pc, cp,
                       (unsigned long long)cpu.count);
                break;
            }
            /* If the same page keeps faulting, the resolver did not actually
               make it present -- stop instead of spinning forever. */
            static u32 last_va; static unsigned repeat;
            if (va == last_va) {
                if (++repeat > 4) {
                    printf("[ufault] %08x (%s) still faulting after %u "
                           "resolutions -- giving up @%llu\n", ufault_va,
                           ufault_code ? "code" : "data", repeat,
                           (unsigned long long)cpu.count);
                    break;
                }
            } else { last_va = va; repeat = 0; }
            if (ufaults++ < 24)
                printf("[ufault] %08x (%s) resolved\n", ufault_va,
                       ufault_code ? "code" : "data");
            continue;                       /* pc unchanged: re-execute */
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
