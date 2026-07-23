/* Generated split of the former single-file nx88.c.
   Shared types, macros, hot inline accessors, and all
   cross-module declarations live in nx88.h. */
#include "nx88.h"

u32 upool_alloc(void) { u32 p = upool_next; upool_next += PAGE_SIZE;
                               mem_zero(p, PAGE_SIZE); return p; }

void umap(u32 segtab, u32 va, u32 pa)
{
    u32 sidx = (va >> 22) & 0x3FF;
    u32 sdesc = mem_r32(segtab + sidx * 4), pgtab;
    if (sdesc & 1) pgtab = sdesc & 0xFFFFF000u;
    else { pgtab = upool_alloc(); mem_w32(segtab + sidx * 4, pgtab | 1); }
    mem_w32(pgtab + ((va >> 12) & 0x3FF) * 4, (pa & 0xFFFFF000u) | 1);
}

u32 udemand_page(u32 apr, u32 va)
{
    if (!usegtab_cur || (apr & 0xFFFFF000u) != usegtab_cur) return 0;
    if (va >= 0xC0000000u) return 0;
    u32 pg = upool_alloc();
    umap(usegtab_cur, va, pg);
    tlb_flush();
    if (++udemand_count <= 32)
        if (!quiet_uproc) printf("[upage] demand-mapped va %08x -> pa %08x (pc=%08x)\n",
               va & ~0xFFFu, pg, cpu.pc);
    return pg;
}

/* Write into the synthetic user address space (walking our own segment table). */
void uwrite8(u32 segtab, u32 va, u8 b)
{
    u32 pa;
    if (mmu_walk(segtab | 1u, va, &pa)) mem_w8(pa, b);
}

void uwrite32(u32 segtab, u32 va, u32 v)
{
    uwrite8(segtab, va,     (u8)(v >> 24)); uwrite8(segtab, va + 1, (u8)(v >> 16));
    uwrite8(segtab, va + 2, (u8)(v >> 8));  uwrite8(segtab, va + 3, (u8)v);
}

int load_user_prog_av(const char *path, u32 segtab, u32 *entry, u32 *sp_out,
                             const char **av, unsigned nav,
                             const char **ev, unsigned nev)
{
    AOut a;
    if (aout_load(path, &a)) return -1;
    u32 dv = (a.text + 4095) & ~4095u;
    u32 total = dv + a.data + a.bss;

    for (u32 va = 0; va < total; va += PAGE_SIZE) {
        u32 pg = upool_alloc();                       /* zeroed */
        umap(segtab, va, pg);
        for (u32 i = 0; i < PAGE_SIZE; i++) {
            u32 o = va + i; u8 b = 0;
            if (o < a.text)                       b = a.img[HDR_PAGE + o];
            else if (o >= dv && o < dv + a.data)  b = a.img[HDR_PAGE + a.text + (o - dv)];
            mem_w8(pg + i, b);                        /* bss stays zero */
        }
    }
    /* stack pages just below STACK_TOP */
    u32 top = STACK_TOP;
    for (u32 i = 0; i < 8; i++) umap(segtab, top - (i + 1) * PAGE_SIZE, upool_alloc());

    /* Build the standard BSD startup stack: the strings at the very top, then
       { argc, argv[0..n-1], NULL, envp[0..m-1], NULL } that crt0 unpacks. */
    u32 sp = top;
    u32 sptr[MAX_UARGV + MAX_UENVP + 2];
    unsigned ns = 0;
    for (unsigned k = 0; k < nav; k++) {
        u32 l = (u32)strlen(av[k]) + 1;
        sp -= l;
        for (u32 i = 0; i < l; i++) uwrite8(segtab, sp + i, (u8)av[k][i]);
        sptr[ns++] = sp;
    }
    for (unsigned k = 0; k < nev; k++) {
        u32 l = (u32)strlen(ev[k]) + 1;
        sp -= l;
        for (u32 i = 0; i < l; i++) uwrite8(segtab, sp + i, (u8)ev[k][i]);
        sptr[ns++] = sp;
    }
    u32 nwords = 1 + nav + 1 + nev + 1;
    sp = (sp - nwords * 4) & ~7u;
    u32 w = sp;
    uwrite32(segtab, w, nav); w += 4;
    for (unsigned k = 0; k < nav; k++)  { uwrite32(segtab, w, sptr[k]); w += 4; }
    uwrite32(segtab, w, 0); w += 4;
    for (unsigned k = 0; k < nev; k++)  { uwrite32(segtab, w, sptr[nav + k]); w += 4; }
    uwrite32(segtab, w, 0);

    *entry = a.entry; *sp_out = sp;
    if (!quiet_uproc) printf("[uproc] loaded %s: text=%u data=%u@%08x bss=%u entry=%08x sp=%08x argc=%u\n",
           path, a.text, a.data, dv, a.bss, a.entry, sp, nav);
    return 0;
}

/* Map a guest path onto the extracted tape directory that mirrors the root
   filesystem the kernel is reading blocks from. */
const char *guest_path(const char *p)
{
    static char buf[1024];
    snprintf(buf, sizeof buf, "%s%s%s", guest_root,
             (*p == '/') ? "" : "/", p);
    return buf;
}

int uread_str(u32 va, char *buf, size_t n)
{
    size_t i = 0;
    for (; i + 1 < n; i++) {
        buf[i] = (char)mem_r8(translate(va + (u32)i, 0));
        if (!buf[i]) return (int)i;
    }
    buf[i] = 0;
    return (int)i;
}

/* Deep-copy an address space: every valid segment, every valid page. */
u32 aspace_clone(u32 src)
{
    u32 dst = upool_alloc();
    for (u32 s = 0; s < 1024; s++) {
        u32 sd = mem_r32(src + s * 4);
        if (!(sd & 1)) continue;
        u32 spt = sd & 0xFFFFF000u, dpt = upool_alloc();
        mem_w32(dst + s * 4, dpt | 1);
        for (u32 p = 0; p < 1024; p++) {
            u32 pe = mem_r32(spt + p * 4);
            if (!(pe & 1)) continue;
            u32 pg = upool_alloc(), sp = pe & 0xFFFFF000u;
            for (u32 i = 0; i < PAGE_SIZE; i += 4) mem_w32(pg + i, mem_r32(sp + i));
            mem_w32(dpt + p * 4, pg | 1);
        }
    }
    return dst;
}

void uproc_activate(u32 segtab)
{
    usegtab_cur = segtab;
    /* Both APRs: pmap_activate sets user and supervisor alike, so the kernel's
       copyin/copyout of syscall arguments reaches the user program.  Kernel
       addresses bypass the APR entirely under realmm, so this is safe. */
    mem_w32(0xFFF7E000u + CMMU_UAPR, segtab | 1);
    mem_w32(0xFFF7F000u + CMMU_UAPR, segtab | 1);
    mem_w32(0xFFF7E000u + CMMU_SAPR, segtab | 1);
    mem_w32(0xFFF7F000u + CMMU_SAPR, segtab | 1);
    tlb_flush();
}

void uarea_dup_files(void)
{
    for (int i = 0; i < U_NOFILE; i++) {
        u32 f = mem_r32(UAREA_VA + U_OFILE + 4 * i);
        if (f < 0xC0000000u || f >= 0xC8000000u) continue;
        u32 pa = translate(f + F_COUNT, 0);
        mem_w16(pa, (u16)(mem_r16(pa) + 1));
    }
}

/* --------------------------------------------------------------------------
   Per-process u-area virtualization for the kernel's OWN context switches.

   The u-area lives behind a fixed kernel VA window: the struct at 0xFBFFE000
   (curproc, u_ofile, ...) and the kernel stack at 0xFBFFD000, double-mapped by
   load_context into [0xFBFFC000, 0xFC000000).  On real hardware load_context
   (_local_phys_pte_page, c0017498) rewrites the per-node page-table slot so the
   window's two physical pages become the incoming process's own u-area pages
   (named in ctx+0x98).  Our translate() short-circuits that whole window to
   identity, so every process shares one physical u-area -- which is why a
   process switched out mid-kernel-call (e.g. the SCSI probe parked in procdup)
   resumes on a kernel stack that intervening processes have overwritten, reads
   a zero return address at [r31+0x11c], and jumps to 0.

   We virtualize the window instead of remapping it: at each load_context we
   save the outgoing process's window content into a slot keyed by its u98 and
   restore the incoming one's.  The physical ctx+0x98 pages are never written by
   the kernel in our model (its u-area writes all land in the identity window),
   so they can't serve as storage -- hence the side table.  A u98 we have never
   seen is a freshly forked child: we leave the window untouched so it inherits
   the parent's just-copied kernel stack, exactly as the shared-window model
   already (accidentally) did correctly for children. */
/* Virtualize only the kernel-STACK page (0xFBFFD000).  The struct page at
   0xFBFFE000 (curproc, u_ofile, ...) is kept shared/identity: the kernel writes
   curproc explicitly on every switch, and the rest of the emulator already
   treats that page as the live u-area, so swapping it per-process instead breaks
   the root-mount path.  The derail we are fixing is purely a stale kernel stack
   ([r31+0x11c] read back as 0), which lives entirely on the stack page. */
#define UWIN_VA   0xFBFFD000u
#define UWIN_SIZE 0x1000u
#define MAX_USLOT 512
static struct uslot { u32 u98; u8 data[UWIN_SIZE]; } uslot[MAX_USLOT];
static int nuslot;
u32 cur_u98;

static int uslot_find(u32 u98)
{
    for (int i = 0; i < nuslot; i++) if (uslot[i].u98 == u98) return i;
    return -1;
}

static int uslot_get(u32 u98)
{
    int s = uslot_find(u98);
    if (s >= 0) return s;
    if (nuslot >= MAX_USLOT) return -1;         /* table full: drop the save */
    uslot[nuslot].u98 = u98;
    return nuslot++;
}

static void uwin_save(u8 *dst)
{
    for (u32 i = 0; i < UWIN_SIZE; i += 4)
        *(u32 *)(dst + i) = mem_r32(UWIN_VA + i);
}

static void uwin_load(const u8 *src)
{
    for (u32 i = 0; i < UWIN_SIZE; i += 4)
        mem_w32(UWIN_VA + i, *(const u32 *)(src + i));
}

/* Called at load_context entry (pc == 0xc0017498) with the incoming context. */
void lctx_switch(u32 ctx)
{
    if (!realu) return;
    u32 new98 = mem_r32(translate(ctx + 0x98, 0));
    if (!new98 || new98 == cur_u98) return;
    if (cur_u98) {                              /* bank the outgoing process   */
        int s = uslot_get(cur_u98);
        if (s >= 0) uwin_save(uslot[s].data);
    }
    int s = uslot_find(new98);
    if (s >= 0) uwin_load(uslot[s].data);       /* restore a known process...  */
    /* ...otherwise leave the window: a new child inherits the parent's stack. */
    cur_u98 = new98;
    if (lct_trace) {
        u32 node = mem_r32(translate(0xc0014008u, 0)) & 0xFFFFu;
        u32 pteaddr = mem_r32(translate(0xc1015548u + node * 4, 0));
        u32 old98 = pteaddr ? (mem_r32(translate(pteaddr, 0)) & 0xFFFFF000u) : 0;
        printf("[lct] switch->u98=%08x rpc=%08x (%s, %d slots) pte_old98=%08x @%llu\n",
               new98, mem_r32(translate(ctx + 0x80, 0)),
               s >= 0 ? "restored" : "inherited", nuslot, old98,
               (unsigned long long)cpu.count);
    }
}

void uarea_save(UProc *u)
{
    for (u32 i = 0; i < UAREA_SIZE; i++) u->uarea[i] = mem_r8(UAREA_VA + i);
    memcpy(u->fdcon, fd_console, sizeof fd_console);
    memcpy(u->fdpipe, fd_pipe, sizeof fd_pipe);
}

void uarea_load(const UProc *u)
{
    for (u32 i = 0; i < UAREA_SIZE; i++) mem_w8(UAREA_VA + i, u->uarea[i]);
    memcpy(fd_console, u->fdcon, sizeof fd_console);
    memcpy(fd_pipe, u->fdpipe, sizeof fd_pipe);
}

void uctx_save(u32 pc)
{
    if (ucur < 0) return;
    UProc *u = &uprocs[ucur];
    u->pc = pc;
    for (int i = 0; i < 32; i++) u->r[i] = RD(i);
    uarea_save(u);
}

void uctx_load(int i)
{
    UProc *u = &uprocs[i];
    ucur = i;
    for (int k = 1; k < 32; k++) WR(k, u->r[k]);
    cpu.pc = u->pc;
    cpu.cr[1] = 0;                                  /* user mode */
    uarea_load(u);
    uproc_activate(u->segtab);
}

int uproc_new(int ppid, u32 segtab)
{
    for (int i = 0; i < MAX_UPROC; i++)
        if (!uprocs[i].used) {
            memset(&uprocs[i], 0, sizeof uprocs[i]);
            uprocs[i].used = 1;
            uprocs[i].pid = next_pid++;
            uprocs[i].ppid = ppid;
            uprocs[i].segtab = segtab;
            return i;
        }
    return -1;
}

int uproc_runnable(void)
{
    for (int i = 0; i < MAX_UPROC; i++)
        if (uprocs[i].used && !uprocs[i].zombie && !uprocs[i].waiting) return i;
    return -1;
}

int uproc_find_pid(int pid)
{
    for (int i = 0; i < MAX_UPROC; i++)
        if (uprocs[i].used && uprocs[i].pid == pid) return i;
    return -1;
}

/* Service the process-lifetime syscalls.  Returns 1 if handled; the pc is left
   at tpc+8 (success) or tpc+4 (error) exactly as the kernel's return path
   would leave it. */
int uproc_syscall(u32 sysno, u32 tpc)
{
    if (!uproc_on || ucur < 0) return 0;
    UProc *me = &uprocs[ucur];
    u32 a0 = RD(2), a1 = RD(3), a2 = RD(4);

    /* Mach traps come in on the same vector with a negative number, and the
       kernel routes them through mach_trap_table -- which in this build is
       kern_invalid for all but one entry.  Delivering them for real instead
       walks off the front of sysent into whatever function happens to lie
       there (wait1, as it turns out) and panics.  Their stubs have no error
       branch, so the return is tpc+4 with the status in r2. */
    if ((s32)sysno < 0) {
        if (trace_traps)
            printf("[uproc] pid %d mach trap %d -> KERN_INVALID_ARGUMENT\n",
                   me->pid, (int)(s32)sysno);
        WR(2, 4);                                  /* KERN_INVALID_ARGUMENT */
        cpu.pc = tpc + 4;
        return 1;
    }

    switch (sysno) {
    case 20: WR(2, me->pid);  cpu.pc = tpc + 8; return 1;   /* getpid  */
    case 39: WR(2, me->ppid); cpu.pc = tpc + 8; return 1;   /* getppid */

    /* obreak.  The heap has to be ours because the address space is: our
       processes all share proc0's vm_map, so the kernel's break is a single
       shared value.  The first program to grow the heap moved it, and the next
       one's opening obreak -- asking for a lower address than the shared map
       had reached -- was refused, which is why a second `ls' died with "out of
       memory".  Demand paging backs whatever the program then touches. */
    case 17:
        /* ...except the bootstrap stub's one real obreak, which is how the
           kernel learns the data segment is big enough for useracc. */
        if (brk_passthru || (tpc & ~0xFFFu) == UBOOT_VA) {
            brk_watch_pc = tpc; brk_watch_arg = a0;
            return 0;
        }
        me->brk = a0;
        WR(2, 0);
        cpu.pc = tpc + 8;
        return 1;

    case 2: case 66: {                                      /* fork/vfork */
        u32 child_as = aspace_clone(me->segtab);
        int ci = uproc_new(me->pid, child_as);
        if (ci < 0) { WR(2, 12); cpu.pc = tpc + 4; return 1; }   /* ENOMEM */
        UProc *c = &uprocs[ci];
        for (int i = 0; i < 32; i++) c->r[i] = RD(i);
        /* Two-value return.  libc's fork wrapper is `bcnd eq0, r3, keep-r2`
           followed by `r2 = 0` -- so r3 == 0 means "you are the parent, r2 is
           the child's pid" and r3 != 0 means "you are the child".  Leaving r3
           at whatever the caller had makes BOTH sides run the child's path. */
        c->r[2] = (u32)me->pid;
        c->r[3] = 1;                                        /* child  */
        c->pc = tpc + 8;
        uarea_dup_files();          /* the child gets a reference too... */
        uarea_save(c);              /* ...and its own copy of the table   */
        if (!quiet_uproc) printf("[uproc] pid %d forked -> pid %d (aspace %08x)\n",
               me->pid, c->pid, child_as);
        /* Run the CHILD first.  Processes here run one at a time, so whoever
           runs first must be the one that can make progress -- and after a
           fork that is always the child.  Let the parent go on instead and it
           reads its half of the pipe while the writer has not run yet; the
           kernel then tries to sleep on behalf of what it still believes is
           the idle process, and panics.  So the parent's fork return is
           banked here and taken up when the child exits or blocks. */
        WR(2, (u32)c->pid);
        WR(3, 0);                                           /* parent */
        uctx_save(tpc + 8);
        uctx_load(ci);
        return 1;
    }

    case UBOOT_DONE: {          /* bootstrap stub finished its three opens */
        u32 fresh = upool_alloc(), entry, sp;
        u32 save = usegtab_cur;
        usegtab_cur = 0;
        if (!nuargv) uargv[nuargv++] = uprog_path;
        int rc = load_user_prog_av(uprog_path, fresh, &entry, &sp,
                                   uargv, nuargv, uenvp, nuenvp);
        usegtab_cur = save;
        if (rc) { fprintf(stderr, "[uproc] could not load %s\n", uprog_path);
                  uproc_all_done = 1; return 1; }
        me->segtab = fresh;
        for (int i = 1; i < 32; i++) WR(i, 0);
        WR(31, sp);
        cpu.pc = entry;
        uproc_activate(fresh);
        if (dump_uarea) {
            printf("[uarea] u-area +0x730..+0x790:\n");
            for (u32 o = 0x730; o < 0x790; o += 4)
                printf("   +%04x = %08x\n", o, mem_r32(UAREA_VA + o));
            for (int k = 0; k < 3; k++) {
                u32 f = mem_r32(UAREA_VA + 0x748 + 4 * k);
                printf("[uarea] file[%d] @%08x:", k, f);
                for (u32 o = 0; o < 0x3c; o += 4)
                    printf(" %08x", mem_r32(translate(f + o, 0)));
                printf("\n");
            }
        }
        return 1;
    }

    case 59: {                                              /* execve */
        char path[512];
        uread_str(a0, path, sizeof path);
        const char *av[MAX_UARGV], *ev[MAX_UENVP];
        static char strbuf[MAX_UARGV + MAX_UENVP][256];
        unsigned nav = 0, nev = 0;
        for (u32 p = a1; a1 && nav < MAX_UARGV; p += 4) {
            u32 sp = mem_r32(translate(p, 0));
            if (!sp) break;
            uread_str(sp, strbuf[nav], sizeof strbuf[0]);
            av[nav] = strbuf[nav]; nav++;
        }
        for (u32 p = a2; a2 && nev < MAX_UENVP; p += 4) {
            u32 sp = mem_r32(translate(p, 0));
            if (!sp) break;
            uread_str(sp, strbuf[MAX_UARGV + nev], sizeof strbuf[0]);
            ev[nev] = strbuf[MAX_UARGV + nev]; nev++;
        }
        if (!nav) { av[0] = path; nav = 1; }

        u32 fresh = upool_alloc(), entry, sp;
        u32 save = usegtab_cur;
        usegtab_cur = 0;              /* no demand paging while we build it */
        int rc = load_user_prog_av(guest_path(path), fresh, &entry, &sp,
                                   av, nav, ev, nev);
        usegtab_cur = save;
        if (rc) {
            if (!quiet_uproc) printf("[uproc] pid %d execve(\"%s\") -> ENOENT\n", me->pid, path);
            WR(2, 2); cpu.pc = tpc + 4; return 1;           /* ENOENT */
        }
        if (!quiet_uproc) printf("[uproc] pid %d execve(\"%s\") argc=%u\n", me->pid, path, nav);
        me->segtab = fresh;
        for (int i = 1; i < 32; i++) WR(i, 0);
        WR(31, sp);
        cpu.pc = entry;
        uproc_activate(fresh);
        return 1;
    }

    case 1: {                                               /* exit(status) */
        me->zombie = 1;
        me->status = (int)((a0 & 0xff) << 8);
        if (!quiet_uproc) printf("[uproc] pid %d exited(%u)\n", me->pid, a0 & 0xff);
        int pi = uproc_find_pid(me->ppid);
        if (pi >= 0 && uprocs[pi].waiting) {                /* wake the parent */
            uprocs[pi].waiting = 0;
            uprocs[pi].r[2] = (u32)me->pid;
            uprocs[pi].r[3] = (u32)me->status;
            if (uprocs[pi].wait_statusp)
                uwrite32(uprocs[pi].segtab, uprocs[pi].wait_statusp, (u32)me->status);
            me->used = 0;                                   /* reaped */
            uctx_load(pi);
            return 1;
        }
        int n = uproc_runnable();
        if (n < 0) { uproc_all_done = 1; return 1; }
        uctx_load(n);
        return 1;
    }

    case 7: case 84: {                                      /* wait4 / wait */
        /* Syscall 7 is wait4(pid, statusp, options, rusage) -- the status
           POINTER is the second argument, and the shell reads $? out of it.
           Writing the status only to r3 left $? stuck at 0 for every command,
           which matters: the install script branches on `case $?'.  Syscall 84
           is the older wait(statusp), status in the first argument. */
        int   want  = (sysno == 7) ? (int)(s32)a0 : -1;
        u32   stptr = (sysno == 7) ? a1 : a0;
        for (int i = 0; i < MAX_UPROC; i++)
            if (uprocs[i].used && uprocs[i].zombie && uprocs[i].ppid == me->pid
                && (want <= 0 || uprocs[i].pid == want)) {
                int pid = uprocs[i].pid, st = uprocs[i].status;
                uprocs[i].used = 0;
                WR(2, (u32)pid); WR(3, (u32)st);
                if (stptr) uwrite32(me->segtab, stptr, (u32)st);
                cpu.pc = tpc + 8;
                return 1;
            }
        int kids = 0;
        for (int i = 0; i < MAX_UPROC; i++)
            if (uprocs[i].used && uprocs[i].ppid == me->pid
                && (want <= 0 || uprocs[i].pid == want)) kids++;
        if (!kids) { WR(2, 10); cpu.pc = tpc + 4; return 1; }   /* ECHILD */
        uctx_save(tpc + 8);                                 /* block, run a child */
        me->waiting = 1;
        me->wait_statusp = stptr;
        int n = uproc_runnable();
        if (n < 0) { uproc_all_done = 1; return 1; }
        uctx_load(n);
        return 1;
    }
    }
    return 0;
}

/* Synthesize proc1: a real, scheduler-selectable process whose saved context
   resumes directly in USER mode.  Rather than build a proc struct field-by-field
   (most of the Mach layout is unknown), clone proc0's proc + context as a
   template so every field we don't understand stays plausible, then override
   only what makes it a user process:
     ctx+0x80 = resume PC (load_context puts it in SNIP and rte's there)
     ctx+0x8c = EPSR with bit31 CLEAR -> the rte lands in user mode
     ctx+0x7c = r31 = user stack pointer
   Finally splice it onto the head of the run queue (circular list, head at
   0xC0014498, forward link [proc+0], back link [proc+4]) so _swtch_pri picks it
   instead of proc0 -- which is what made the previous switch a no-op self-switch. */
u32 create_proc1(u32 user_pc, u32 user_sp, u32 usegtab)
{
    u32 cp   = mem_r32(translate(0xFBFFE0F0u, 0));   /* proc0            */
    u32 cctx = mem_r32(translate(cp + 0xc8, 0));     /* proc0's context  */

    u32 P = upool_next; upool_next += 0x4000u;       /* proc + u-area    */
    u32 C = upool_next; upool_next += PAGE_SIZE;     /* saved context    */

    for (u32 i = 0; i < 0x4000u; i += 4)             /* clone proc0      */
        mem_w32(translate(P + i, 0), mem_r32(translate(cp + i, 0)));
    for (u32 i = 0; i < 0x100u; i += 4)              /* clone its context*/
        mem_w32(translate(C + i, 0), mem_r32(translate(cctx + i, 0)));

    mem_w32(translate(P + 0xc8, 0), C);              /* proc1 -> context */
    mem_w32(translate(C + 0x80, 0), user_pc);        /* resume PC        */
    mem_w32(translate(C + 0x8c, 0), 0u);             /* EPSR: user mode  */
    mem_w32(translate(C + 0x7c, 0), user_sp);        /* user SP          */
    /* Address space: load_context calls pmap_activate (_dummy_use_value
       c00a30c0) with r2 = ctx+0xa4, which does UAPR/SAPR = [ctx+0xa4] |
       0x80000000.  Point it at our synthetic user segment table so the
       switched-to process sees the user program. */
    mem_w32(translate(C + 0xa4, 0), usegtab | 1u);
    /* Give proc1 its OWN u-area.  load_context maps the u-area by writing the
       two physical pages named in ctx+0x98 into a fixed per-node page-table
       slot -- the classic BSD arrangement where the u-area lives at a constant
       kernel VA and the switch remaps the physical pages behind it.  Cloning
       proc0's u-area content gives proc1 a structurally valid PCB/kernel stack
       to start from; the trap's saveregs then builds its own frame there. */
    {
        u32 u0 = mem_r32(translate(cctx + 0x98, 0));   /* proc0's u-area (phys) */
        u32 U  = upool_next; upool_next += 0x2000u;    /* 2 pages, physical     */
        /* ZERO it rather than clone.  fpipe only writes the DMT/fault slots of
           the trap frame when there is a valid data transaction; anything it
           skips keeps whatever was already in the u-area.  Cloned garbage there
           reads back as pending FP/data exceptions, and exreturn then returns
           non-zero -- making trap() bail before it ever dispatches the syscall. */
        for (u32 i = 0; i < 0x2000u; i += 4) mem_w32(U + i, 0);
        mem_w32(translate(C + 0x98, 0), U);
        if (!quiet_uproc) printf("[utest]   proc1 u-area %08x (zeroed; proc0's was %08x)\n", U, u0);
    }

    /* splice onto the head of the run queue so swtch_pri selects proc1 */
    u32 rq = 0xC0014498u;
    u32 first = mem_r32(translate(rq, 0));
    mem_w32(translate(P + 0, 0), first);
    mem_w32(translate(P + 4, 0), rq);
    if (first >= 0xC0000000u) mem_w32(translate(first + 4, 0), P);
    mem_w32(translate(rq, 0), P);

    if (!quiet_uproc) printf("[utest] proc1=%08x ctx=%08x (cloned from proc0=%08x/%08x), "
           "queued at runq head\n", P, C, cp, cctx);
    if (!quiet_uproc) printf("[utest]   ctx +7c(sp)=%08x +80(pc)=%08x +8c(epsr)=%08x\n"
           "[utest]   ctx +98(uarea)=%08x +a0(pmap)=%08x +a4(aspace)=%08x +dc=%08x\n",
           mem_r32(translate(C + 0x7c, 0)), mem_r32(translate(C + 0x80, 0)),
           mem_r32(translate(C + 0x8c, 0)),
           mem_r32(translate(C + 0x98, 0)), mem_r32(translate(C + 0xa0, 0)),
           mem_r32(translate(C + 0xa4, 0)), mem_r32(translate(C + 0xdc, 0)));
    return P;
}

void launch_utest(void)
{
    u32 segtab  = upool_alloc();
    u32 codepa  = upool_alloc();
    u32 stackpa = upool_alloc();
    u32 uva = 0x1000u;
    /* getpid (syscall 20), then spin at the error and success return points */
    /* Syscall trap vector = 128, exactly as nX user binaries issue it
       (`or r9,r0,N; tb0 0,r0,128`; see ../tapeimage/bin/echo).  Vector 0x73
       (_Xsyscall) is a separate trap whose trap() case just runs the exception
       exit path.  --utrap=N overrides for experiments. */
    static u32 prog[] = {
        0x59200014u,   /* or  r9, r0, 20   ; getpid            */
        0xf000d073u,   /* tb0 0, r0, 0x73  ; syscall trap      */
        0xc0000000u,   /* br  .            ; error   (pc+4)    */
        0xc0000000u,   /* br  .            ; success (pc+8)    */
    };
    prog[1] = 0xf000d000u | (utrap_vec & 0x1FFu);
    for (unsigned i = 0; i < sizeof prog / 4; i++) mem_w32(codepa + i * 4, prog[i]);
    umap(segtab, uva, codepa);
    umap(segtab, 0xE000u, stackpa);
    /* --uprog=PATH: run a real nX binary from the extracted tape instead of the
       4-instruction probe.  Overrides the entry PC and stack. */
    u32 usp = 0xF000u;
    if (uprog_path) {
        /* Before the program runs, open the three standard descriptors for
           real.  The process starts with an empty descriptor table, so an
           un-bootstrapped pipe() is handed 0 and 1 and collides head-on with
           the console -- the shell then dup2's its pipeline over stdin/stdout
           and everything unravels.  Three genuine kernel opens put real file
           structs in slots 0-2, so pipe() starts at 3 where it belongs.

           The opens have to be issued from user mode, so they are a short
           stub the process executes before its entry point; it ends with the
           private syscall UBOOT_DONE, which loads the real program. */
        u32 stub = upool_alloc();
        umap(segtab, UBOOT_VA, stub);
        static const char devnull[] = "/dev/null";
        for (unsigned i = 0; i < sizeof devnull; i++)
            mem_w8(stub + 0x80 + i, (u8)devnull[i]);
        u32 pathva = UBOOT_VA + 0x80, w = 0;
        for (int k = 0; k < 3; k++) {
            mem_w32(stub + w, 0x5C400000u | (pathva >> 16));         w += 4;
            mem_w32(stub + w, 0x58420000u | (pathva & 0xFFFF));      w += 4;
            mem_w32(stub + w, 0x58600002u);   /* or r3, r0, 2  O_RDWR */ w += 4;
            mem_w32(stub + w, 0x59200005u);   /* or r9, r0, 5  open  */ w += 4;
            mem_w32(stub + w, 0xf000d080u);   /* tb0 0, r0, 128      */ w += 4;
            mem_w32(stub + w, 0xc0000001u);   /* br +1: error -> on  */ w += 4;
        }
        /* One real obreak, to a break far above anything a program will ask
           for.  The heap itself is ours (see the obreak case in
           uproc_syscall), but the kernel still validates user pointers against
           the data segment it believes the process has -- useracc, which
           sigreturn uses.  longjmp here IS sigreturn, with the sigcontext
           sitting in the program's data, so without this every longjmp fails
           and the shell reports "longjmp botch".  Granting the segment once
           covers every process, since they share the one vm_map. */
        mem_w32(stub + w, 0x5C400000u | (UBRK_TOP >> 16));           w += 4;
        mem_w32(stub + w, 0x58420000u | (UBRK_TOP & 0xFFFF));        w += 4;
        mem_w32(stub + w, 0x59200011u);       /* or r9, r0, 17  obreak */ w += 4;
        mem_w32(stub + w, 0xf000d080u);                              w += 4;
        mem_w32(stub + w, 0xc0000001u);       /* br +1: error -> on  */ w += 4;

        mem_w32(stub + w, 0x59200000u | UBOOT_DONE);                 w += 4;
        mem_w32(stub + w, 0xf000d080u);                              w += 4;
        mem_w32(stub + w, 0xc0000000u);       /* br . (never reached) */
        uva = UBOOT_VA;
        usp = STACK_TOP - 0x100u;             /* demand-paged */
    }
    uproc_activate(segtab);                         /* APRs + demand paging */
    /* Register it as pid 1 of the emulator-managed process table, so its
       fork/exec/wait/exit are serviced (see the process-management notes). */
    {
        uproc_on = 1;
        int i = uproc_new(0, segtab);
        uprocs[i].pid = 1; next_pid = 2;
        ucur = i;
    }
    /* A trap from user mode switches the kernel to the stack in SR1(cr17):
       fpipe does `ld r31,cr17`.  We must point it at a valid kernel stack or
       the register save (and thus the read-back syscall number) lands in
       garbage -> the syscall dispatches to kern_invalid.  Use proc0's own
       kernel stack (its current r31) as the trap stack. */
    /* Integrate proc0's thread into the per-node curproclist the scheduler
       maintains -- add_curproc (_fix_misaligned_flag c004f454) does: node at
       [thread+0x78], chain [thread+0x14]=curproclist[node], curproclist[node]=
       thread (head at [0xc101f430 + node*4]).  proc0 was made curproc manually
       and never added, so remove_curproc (in the trap-return reschedule) can't
       find it and panics.  Add its thread (curproc+0x200) at node 0. */
    {
        u32 cp = mem_r32(translate(0xFBFFE0F0u, 0));
        u32 thr = cp + 0x200u;
        u32 node = 0;
        u32 head = 0xC101F430u + node * 4;
        mem_w16(translate(thr + 0x78, 0), (u16)node);
        mem_w32(translate(thr + 0x14, 0), mem_r32(translate(head, 0)));
        mem_w32(translate(head, 0), thr);
        if (!quiet_uproc) printf("[utest] added thread %08x to curproclist[%u]\n", thr, node);
    }
    /* Create proc1: a real scheduler-selectable process that resumes in user
       mode at the same program.  When the syscall's trap-return reschedule
       runs, _swtch_pri should now pick proc1 (queued ahead of proc0) and
       load_context it -- landing in user mode as a properly scheduled thread. */
    create_proc1(uva, usp, segtab);

    cpu.cr[17] = RD(31);                            /* SR1 = kernel stack */
    WR(31, usp);                                    /* user stack pointer */
    cpu.pc = uva;
    cpu.cr[1] = 0u;                                 /* user mode, interrupts on */
    if (interactive)
        printf("\n=== nX on the TC2000 -- /bin/sh from the 1989 install tape ===\n"
               "The root filesystem is the tape's own UFS.  ^D or `exit' quits.\n\n");
    if (!quiet_uproc) printf("[utest] user @VA %08x segtab=%08x kstack(cr17)=%08x; dropping to "
           "user mode\n", uva, segtab, cpu.cr[17]);
}
