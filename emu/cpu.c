/* Generated split of the former single-file nx88.c.
   Shared types, macros, hot inline accessors, and all
   cross-module declarations live in nx88.h. */
#include "nx88.h"

/* ---- temporary VM-probe instrumentation (step-1 feasibility) ----
   Count entries to the kernel's fault/VM machinery so we can see which of it is
   actually alive during boot and under a running process.  --vmprobe enables it;
   vm_probe_report() prints at halt.  Also logs the first few vm_map_fault calls
   with their args and CPU mode, to see what the kernel is being asked to resolve. */
typedef struct { u32 addr; const char *name; u64 count, first; } VmProbe;
static VmProbe vmprobes[] = {
    /* The symbol table's _X* names are shifted one slot: c0016000 is really
       the interrupt handler, c0016180 _Xcodaccess, c0016190 _Xdataccess.
       Named here by what they actually are (see deliver_fault). */
    {0xC0016000u, "Xinterrupt",           0, 0},
    {0xC0016180u, "Xcodaccess",           0, 0},
    {0xC0016190u, "Xdataccess",           0, 0},
    {0xC0017788u, "trap",                 0, 0},
    {0xC008B35Cu, "vm_map_fault",         0, 0},
    {0xC0090B2Cu, "vm_map_fault_foreign", 0, 0},
    {0xC0091224u, "pmap_enter",           0, 0},
    {0xC008B5E8u, "pmap_expand",          0, 0},
    {0xC004F9BCu, "vm_map_create",        0, 0},
    {0xC0094984u, "vm_allocate",          0, 0},
    {0xC00A6AB0u, "vm_page_alloc",        0, 0},
    {0xC0094058u, "pmap_create",          0, 0},
    {0xC0096610u, "vm_object_create",     0, 0},
    {0xC00978FCu, "vnode_pager_init",     0, 0},
    {0xC0017138u, "copyin",               0, 0},
    {0xC00170F0u, "copyout",              0, 0},
    /* process creation / exec / first-user-process bootstrap */
    {0xC004D188u, "fork",                 0, 0},
    {0xC0057474u, "fork1",                0, 0},
    {0xC0057520u, "procdup",              0, 0},
    {0xC005E5E4u, "vm_map_fork",          0, 0},
    {0xC0078460u, "execve",               0, 0},
    {0xC005E9B8u, "getxfile",             0, 0},
    {0xC0016E30u, "icode(read)",          0, 0},
};
static u64 vmfault_logged;
void vm_probe_tick(u32 pc)
{
    for (unsigned i = 0; i < sizeof vmprobes / sizeof vmprobes[0]; i++)
        if (vmprobes[i].addr == pc) {
            if (!vmprobes[i].count) vmprobes[i].first = cpu.count;
            vmprobes[i].count++;
            if (pc == 0xC008B35Cu && vmfault_logged < 12) {
                vmfault_logged++;
                printf("[vmflt] vm_map_fault r2=%08x r3=%08x r4=%08x mode=%s "
                       "ret=%08x @%llu\n", RD(2), RD(3), RD(4),
                       (cpu.cr[1] & 0x80000000u) ? "sup" : "USER", RD(1),
                       (unsigned long long)cpu.count);
            }
            break;
        }
}
void vm_probe_report(void)
{
    printf("=== VM probe: kernel VM/fault machinery calls ===\n");
    for (unsigned i = 0; i < sizeof vmprobes / sizeof vmprobes[0]; i++)
        printf("  %-22s %10llu   first@%llu\n", vmprobes[i].name,
               (unsigned long long)vmprobes[i].count,
               (unsigned long long)vmprobes[i].first);
}

/* ---- empirical proc/thread-allocator hunt (step-2) ----
   Symbol names in this kernel mislead (five false leads so far), so find the
   real allocator by observation instead:

     --ctxtrace   every distinct context the kernel ever load_context()s.  Those
                  ARE the running threads, whatever they are called.
     --wmem=A[:L] log every store into a virtual range, with the storing PC and
                  r1 (a one-deep backtrace).  Point it at a context found above
                  and the earliest writers are its allocator and initialiser.
     --regfind=V  log the first times any GPR holds V -- i.e. where a pointer is
                  first produced, which is the allocator's return point.
   All three are off by default and cost one predictable branch when off. */
#define KCTX_MAX 512
static struct { u32 ctx; u64 first, n; u32 from; } kctx[KCTX_MAX];
static unsigned nkctx;
static u32 kr32(u32 va) { return mem_r32(translate(va, 0)); }

void ctx_tick(void)
{
    u32 c = RD(2);
    for (unsigned i = 0; i < nkctx; i++)
        if (kctx[i].ctx == c) { kctx[i].n++; return; }
    if (nkctx >= KCTX_MAX) return;
    kctx[nkctx].ctx = c; kctx[nkctx].first = cpu.count;
    kctx[nkctx].n = 1;   kctx[nkctx].from  = RD(1);
    printf("[ctx] #%-3u %08x  pc=%08x sp=%08x kstk=%08x cmmu=%08x  from=%08x @%llu\n",
           nkctx, c, kr32(c + 0x80), kr32(c + 0x7c), kr32(c + 0x98),
           kr32(c + 0xa0), RD(1), (unsigned long long)cpu.count);
    nkctx++;
}

void ctx_report(void)
{
    printf("=== load_context: %u distinct contexts ===\n", nkctx);
    for (unsigned i = 0; i < nkctx; i++)
        printf("  #%-3u %08x  switches=%-6llu first@%llu\n", i, kctx[i].ctx,
               (unsigned long long)kctx[i].n, (unsigned long long)kctx[i].first);
}

/* store watch: called from memop() before translation, so the range is virtual.
   Two modes: a virtual address range (--wmem), and "every store this function
   makes" (--stwatch=PC), which answers "where does this routine actually write?"
   without having to guess the address first. */
static u64 wmem_n;
int stwatch_active;
static u32 stwatch_ret;
static int stwatch_done;
void stwatch_tick(u32 pc)
{
    if (!stwatch_done && !stwatch_active && pc == stwatch_pc) {
        stwatch_active = 1; stwatch_ret = RD(1);
        printf("[stw] enter %08x r2=%08x r3=%08x r4=%08x r5=%08x ret=%08x\n",
               pc, RD(2), RD(3), RD(4), RD(5), stwatch_ret);
    } else if (stwatch_active && pc == stwatch_ret) {
        stwatch_active = 0; stwatch_done = 1;
        printf("[stw] leave, r2=%08x\n", RD(2));
    }
}
void wmem_tick(u32 sub, u32 D, u32 va)
{
    int inrange = (va >= wmem_lo && va < wmem_hi);
    if (!inrange && !stwatch_active) return;
    int store = (sub >= 0x08 && sub <= 0x0B) || sub <= 0x01;
    if (!store) return;
    if (wmem_n++ >= wmem_max) return;
    printf("[wmem] %08x (+%-4d) <= %08x  pc=%08x r1=%08x r31=%08x @%llu\n",
           va, (int)(va - wmem_lo), RD(D), dbg_pc, RD(1), RD(31),
           (unsigned long long)cpu.count);
}

/* register-value watch: where does this pointer value first come from? */
static u64 regfind_n;
void regfind_tick(u32 pc)
{
    for (unsigned r = 2; r < 32; r++)
        if (cpu.r[r] == regfind_val) {
            if (regfind_n++ < 60)
                printf("[rfind] r%-2u = %08x at pc=%08x r1=%08x @%llu\n",
                       r, regfind_val, pc, RD(1), (unsigned long long)cpu.count);
            return;
        }
}

/* ---- kernel-call RPC (step-2 real-exec scaffolding) ----
   Invoke a kernel function from the emulator: snapshot the CPU, put a sentinel
   in r1 as the return address and the args in r2..r5, run until the function
   returns to the sentinel, read the result (r2), restore the CPU.  Guest MEMORY
   changes persist -- that is the point: these functions allocate kernel
   structures (pmaps, vm_maps, pages) we want to keep.  Meant to be used only at
   a quiescent point (the swapper idle loop), in supervisor mode. */
#define KCALL_RET 0x00DEAD00u
u32 kcall(u32 fn, u32 a2, u32 a3, u32 a4, u32 a5, u32 a6)
{
    CPU save = cpu;
    WR(1, KCALL_RET);
    WR(2, a2); WR(3, a3); WR(4, a4); WR(5, a5); WR(6, a6);
    cpu.pc = fn;
    cpu.has_pending = 0;
    u64 guard = 0;
    int trapped = 0;
    while (cpu.pc != KCALL_RET && guard++ < 30000000ull)
        if (step()) { trapped = 1; break; }
    u32 ret = RD(2);
    int returned = (cpu.pc == KCALL_RET);
    u32 tv = trap_vector, tp = dbg_pc;
    u64 used = guard;
    cpu = save;
    if (returned)
        printf("  [kcall %08x] -> %08x  (%llu steps)\n", fn, ret,
               (unsigned long long)used);
    else
        printf("  [kcall %08x] DID NOT RETURN (%s pc=%08x vec=%d, %llu steps)\n",
               fn, trapped ? "trap" : "runaway", tp, (int)tv,
               (unsigned long long)used);
    return returned ? ret : 0xFFFFFFFFu;
}

/* ---- the kernel's REAL process machinery ----
   Found by runtime tracing, not by symbol name (every symbol below is wrong in
   the a.out table; the kernel's own panic strings name them).  The chain, from
   the boot that creates the 64 per-node idle threads:

     proc_init   c004ef74  builds the per-node proc free lists: proc N lives at
                           procbase + N*512, node = N / procs_per_node
     alloc_proc  c004f148  pops procfree[node]  ("alloc_proc: global_proc_lock
                           not held"); link field is proc+0x0c
     newproc     c004e3b0  (unused, node, cluster, existing) -> 0 ok / 1 fail
                           ("newproc to node %d cluster %d"): allocates the proc,
                           assigns the pid, links allproc + pidhash, calls
                           procdup, then do_setrq -- the new proc is RUNNABLE
     procdup     c005e7a4  (new, parent, node): copies the parent's u-area map,
                           then c004fa60 ("proc_create: no space for kernel
                           stack") allocates the kernel stack from node memory
                           and c00a46e0 zeroes the 0xF0-byte context at its base
     load_context c0017498 (ctx) runs it

   proc struct = 512 bytes; context = 0xF0 bytes at the base of the kernel
   stack.  Fields used below were all confirmed against a live boot. */
#define P_SIZE      512u
#define P_LINK      0x0cu       /* free-list / allproc forward link */
#define P_STAT      0x40u       /* 4 = SIDL, 3 = SRUN */
#define P_PID       0x4cu       /* halfword */
#define P_NODE      0x7cu
#define P_UMAP      0xd4u       /* the u-area map from vm_map_u_area_copy */
#define P_CTX       0xc8u       /* the 0xF0-byte context (= kernel stack base) */
#define G_ALLPROC   0xC1015648u
#define G_PROCBASE  0xC1015758u
#define G_NPROC     0xC1014B80u
#define G_PROCPERND 0xC10157E0u
#define G_NNODES    0xC1014D40u
#define G_NEXTPID   0xC1015A68u
#define G_PROCINUSE 0xC1006B20u
#define G_CURPROC   0xFBFFE0F0u     /* u.u_procp */
#define F_NEWPROC   0xC004E3B0u

static void dump_words(const char *what, u32 base, u32 n)
{
    printf("  %s @%08x:", what, base);
    for (u32 i = 0; i < n; i++) {
        if (i % 8 == 0) printf("\n    +0x%03x:", i * 4);
        printf(" %08x", kr32(base + i * 4));
    }
    printf("\n");
}

/* Walk an address space through the kernel's OWN tables.
   With --dataphys the tables are readable at the physical addresses the APR
   names, so an ordinary hardware-shaped walk works and this is just mmu_walk.
   Without it they are only reachable through the kernel VIRTUAL addresses they
   were built at, so fall back to that: pmap[0] is the segment table's kernel VA
   and pmap[1] its physical address, and their difference reaches the page
   tables too.  Returns the physical frame the kernel intends for va. */
static int kwalk(u32 pmap, u32 va, u32 *pa, u32 *sd_out, u32 *pd_out)
{
    u32 stva = kr32(pmap), stpa = kr32(pmap + 4);
    if (mmu_walk(stpa | 1u, va, pa)) {
        if (sd_out) *sd_out = mem_r32(stpa + ((va >> 22) & 0x3FF) * 4);
        if (pd_out) *pd_out = 0;
        return 1;
    }
    u32 bias = stva - stpa;                       /* kernel-table VA - PA */
    u32 sd = kr32(stva + ((va >> 22) & 0x3FF) * 4);
    if (sd_out) *sd_out = sd;
    if (!(sd & 1)) return 0;
    u32 ptva = (sd & 0x3FFFF000u) + bias;         /* strip the interleave mode */
    u32 pd = kr32(ptva + ((va >> 12) & 0x3FF) * 4);
    if (pd_out) *pd_out = pd;
    if (!(pd & 1)) return 0;
    *pa = (pd & 0xFFFFF000u) | (va & 0xFFF);
    return 1;
}

/* Byte/word stores into a process's address space, through its own tables. */
static void uput8(u32 pmap, u32 va, u8 v)
{
    u32 pa;
    if (kwalk(pmap, va, &pa, 0, 0)) mem_w8(pa, v);
}
static void uput32(u32 pmap, u32 va, u32 v)
{
    for (int i = 0; i < 4; i++) uput8(pmap, va + i, (u8)(v >> (24 - i * 8)));
}

/* Create a real process with the kernel's own newproc(), at the swapper idle
   point.  This is the step the synthetic proc1 model has always faked. */
int proc_experiment(void)
{
    u32 base = kr32(G_PROCBASE), cur = kr32(G_CURPROC);
    printf("=== proc experiment: the kernel's real process allocator ===\n");
    printf("  procbase=%08x nproc=%u procs/node=%u nodes=%u inuse=%u "
           "nextpid=%u\n  curproc=%08x (proc #%u, pid %u)\n",
           base, kr32(G_NPROC), kr32(G_PROCPERND), kr32(G_NNODES),
           kr32(G_PROCINUSE), kr32(G_NEXTPID), cur, (cur - base) / P_SIZE,
           kr32(cur + P_PID) >> 16);

    u32 head = kr32(G_ALLPROC);
    /* Which logical cluster to create the process in.  nX is cluster-based and
       refuses to fork out of the SYSTEM cluster ("fork attempted in system
       logical cluster", c004e154): the gate is cluster_table[c]+0x14, which
       holds the cluster's own id and is therefore 0 only for cluster 0.  But
       newproc ALSO switch-cases the cluster and panics "new process in a free
       cluster" (c004e478) for 1, 4 and 5 -- only 2 and 3 are accepted.  So the
       usable cluster is the first of 2,3 that the table agrees exists.
       cluster_table base is [0xC1016D58], 0xb0 bytes per entry, 64 entries. */
    u32 ctab = kr32(0xC1016D58u), clu = 0;
    for (u32 c = 2; ctab && c <= 3; c++)
        if (kr32(ctab + c * 0xB0u + 0x14u) == c) { clu = c; break; }
    procexp_cluster = clu;
    printf("  cluster table @%08x: running in logical cluster %u%s\n", ctab, clu,
           clu ? "" : " (SYSTEM -- fork will panic)");

    /* Create it in cluster 0: newproc for another cluster dispatches into that
       cluster's processor and never returns to the kcall sentinel.  The proc's
       own cluster id is just a field (newproc stores r4 at proc+0x90), so set
       it afterwards -- that is what the fork gate actually reads. */
    u32 rc   = kcall(F_NEWPROC, 0, 0, 0, 0, 0);      /* newproc(-, node 0, -, -) */
    u32 p    = kr32(G_ALLPROC);
    printf("  newproc(node 0) rc=%u   allproc head %08x -> %08x\n", rc, head, p);
    if (rc || p == head) { printf("  !! no new proc\n"); return 0; }

    if (clu) mem_w32(translate(p + 0x90u, 0), clu);   /* forkable cluster */
    procexp_pid = kr32(p + P_PID) >> 16;
    u32 ctx = kr32(p + P_CTX);
    printf("  NEW PROC %08x = proc #%u, pid %u, stat %u, node %u, umap %08x\n",
           p, (p - base) / P_SIZE, kr32(p + P_PID) >> 16, kr32(p + P_STAT),
           kr32(p + P_NODE), kr32(p + P_UMAP));
    printf("  its context %08x: resume pc=%08x sp=%08x kstack=%08x cmmu=%08x\n",
           ctx, kr32(ctx + 0x80), kr32(ctx + 0x7c), kr32(ctx + 0x98),
           kr32(ctx + 0xa0));
    dump_words("proc", p, 64);
    dump_words("context", ctx, 60);

    /* Address space.  initial_context(map) (c008f55c) is just `return
       map->pmap` -- a one-instruction accessor -- so P+0xd4 is a vm_map and
       map+0x50 is its pmap.  The boot sets ctx+0xa0 = pmap[0] and
       ctx+0xa4 = pmap[1] | 1, and pmap[1]|1 has exactly the shape of the
       kernel's own SAPR: it is this address space's APR (segment-table root
       plus the enable bit).  That is the value to put in UAPR to run the
       process. */
    u32 map  = kr32(p + P_UMAP);
    u32 pmap = kcall(0xC008F55Cu, map, 0, 0, 0, 0);      /* vm_map_pmap(map) */
    printf("  vm_map %08x -> pmap %08x  (map+0x50 = %08x)\n",
           map, pmap, kr32(map + 0x50));
    dump_words("vm_map", map, 24);
    if (pmap >= 0xC0000000u && pmap != 0xFFFFFFFFu) {
        dump_words("pmap", pmap, 24);
        printf("  pmap[0]=%08x (ctx+0xa0=%08x)   pmap[1]|1=%08x (ctx+0xa4=%08x)"
               "  %s\n", kr32(pmap), kr32(ctx + 0xa0), kr32(pmap + 4) | 1u,
               kr32(ctx + 0xa4),
               (kr32(pmap) == kr32(ctx + 0xa0)
                && (kr32(pmap + 4) | 1u) == kr32(ctx + 0xa4)) ? "MATCH" : "differ");
    }
    u32 pmap0 = kr32(kr32(cur + P_UMAP) + 0x50);
    printf("  proc0: vm_map %08x pmap %08x apr %08x    live SAPR=%08x UAPR=%08x\n",
           kr32(cur + P_UMAP), pmap0, kr32(pmap0 + 4) | 1u,
           mem_r32(0xFFF7E000u + CMMU_SAPR), mem_r32(0xFFF7E000u + CMMU_UAPR));

    /* --- wire user pages into the new proc's own address space ---
       vm_allocate(map, &addr, size, flags, node) is c008eb6c: it rejects
       flags & 0x108 and node >= nnodes (node -1 = any), and flags bit 0 means
       "anywhere", so bit 0 clear takes the fixed address from *addr.  The boot
       uses flags 0x90 for the u-area.  The in/out address needs a stable
       kernel-writable word: proc+0xe0 is zero and untouched, so it serves. */
    if (pmap < 0xC0000000u || pmap == 0xFFFFFFFFu) return 0;
    u32 slot = p + 0xe0;   /* a zero, unused proc word: vm_allocate's in/out addr */
    /* Sanity: procdup already mapped this proc's u-area at 0xBFFFE000, so that
       VA must resolve through this APR.  If it does not, the APR is wrong and
       nothing below means anything. */
    /* pmap_activate (c00a30c0, called from load_context with &ctx+0xa4) does
       `apr = *p | 0x80000000` and stores it to SAPR *and* UAPR of every CMMU --
       so kernel and user share one segment table per address space, and the
       real APR is ctx+0xa4 | bit31 (interleaved mode 2). */
    /* pmap_map (c00a6b44) writes its descriptors at pmap[0] + (va>>23)*8 --
       and (va>>23)*8 == (va>>22)*4, so pmap[0] IS an ordinary 88200 segment
       table, written two 4MB segments at a time.  But pmap[0] is a kernel
       VIRTUAL address, while the APR (pmap[1]) is physical: check whether they
       are the same memory, because if not, the hardware never sees these
       tables and only the emulator's synthetic map is keeping the boot alive. */
    /* The walker must reach the kernel's tables where they really are before any
       of this proc's user VAs can resolve.  Set the bias first: the loader below
       writes through it. */
    /* Where the table really is, minus where the APR says it is.  Ask
       translate() rather than assuming VA-KOFF: under --dataphys kernel space
       resolves through the kernel's own tables and the two agree, so the bias
       comes out zero and nothing is needed. */
    ktab_bias = translate(kr32(pmap), 0) - kr32(pmap + 4);
    printf("  kernel table bias = %08x (table VA %08x, kernel PA %08x)\n",
           ktab_bias, kr32(pmap), kr32(pmap + 4));

    /* --- fill the address space ---
       Default: load the a.out image directly.  That works (echo/date/pwd run and
       exit cleanly) but leaves the kernel's OWN bookkeeping -- text/data/stack
       sizes and the break -- unset, so the first sbrk fails and `ls` dies with
       "out of memory" in obreak.
       --procexec instead maps a nine-instruction stub that calls execve() and
       lets the kernel load the binary itself.  That is the real destination, but
       it is BLOCKED: the kernel's own software translation of a user address
       (user_vtop, c00ab278) reads the page tables at the physical addresses it
       believes in, which this emulator does not honour, so copyin of the exec
       path fails.  See the kernel-space coherence note below. */
    static char gpath[1024];
    const char *path = uprog_path ? uprog_path : "/bin/echo";
    static char hostpath[1024];
    {   /* the guest sees the tape directory as /, so strip the host prefix */
        const char *g = path;
        size_t gl = guest_root ? strlen(guest_root) : 0;
        if (gl && !strncmp(g, guest_root, gl)) g += gl;
        snprintf(gpath, sizeof gpath, "%s", *g == '/' ? g : "/bin/echo");
        snprintf(hostpath, sizeof hostpath, "%s%s", guest_root ? guest_root : "",
                 gpath);
        if (!uprog_path) path = hostpath;
    }
    AOut a;
    u32 txtaddr = 0, txtoff = 0, dataddr = 0, datoff = 0, img_hi = 0x2000u;
    if (!procexec) {
        if (aout_load(path, &a)) { printf("  !! cannot load %s\n", path); return 0; }
        /* nX tape a.out: an 8192-byte header page in the file, text at VA 0
           (see load_from_aout in proc.c for the two layouts). */
        int std  = ((a.magic >> 16) & 0xFFu) != 0;
        txtoff   = std ? 0 : HDR_PAGE; txtaddr = std ? HDR_PAGE : 0;
        dataddr  = txtaddr + ((a.text + 4095) & ~4095u);
        datoff   = txtoff + a.text;
        img_hi   = (dataddr + a.data + a.bss + 0x1FFFu) & ~0x1FFFu;
    }
    /* The exec stub must not sit where the image it execs will land.  nX puts
       user text at VA 0 and (as the exec'd programs' own faults show) the stack
       around 0x3FFF****, so park the stub and its scratch stack at 0x20000000,
       clear of both.  A hand-loaded image, by contrast, IS the text at VA 0. */
    u32 ubase = procexec ? 0x20000000u : 0x00000000u;
    if (procexec) a.entry = ubase;
    u32 stk_lo = procexec ? ubase + 0x2000u : STACK_TOP - 0x10000u;
    u32 stk_hi = procexec ? ubase + 0x4000u : STACK_TOP;
    /* Note for the exec path: nX's user stack top is 0xBFFFE000, NOT
       STACK_TOP.  exec pushes argv/envp there with a copyout, and the first
       fault a real process takes is the `st.b` at c00171e4 storing to
       0xBFFFDFE0.  Pre-mapping that range here does NOT help -- exec replaces
       the address space, so anything mapped before it is discarded. */
    struct { u32 lo, hi; const char *what; } rng[2] = {
        { ubase, ubase + img_hi, procexec ? "stub" : "image" },
        { stk_lo, stk_hi,        "stack" },
    };
    for (unsigned i = 0; i < 2; i++) {
        mem_w32(translate(slot, 0), rng[i].lo);
        u32 e = kcall(0xC008EB6Cu, map, slot, rng[i].hi - rng[i].lo,
                      0x90, 0xFFFFFFFFu);
        u32 w = kcall(0xC0092350u, map, rng[i].lo, rng[i].hi, 1, 0);
        printf("  %s %08x..%08x: vm_allocate errno=%u, vm_map_pageable errno=%u\n",
               rng[i].what, rng[i].lo, rng[i].hi, e, w);
        if (e || w) return 0;
    }

    if (!procexec) {   /* copy text/data into the frames the kernel chose */
        u32 missing = 0;
        for (u32 va = 0; va < img_hi; va += PAGE_SIZE) {
            u32 pa = 0;
            if (!kwalk(pmap, va, &pa, 0, 0)) { missing++; continue; }
            for (u32 i = 0; i < PAGE_SIZE; i++) {
                u32 o = va + i;
                u8 b = 0;
                if (o >= txtaddr && o < txtaddr + a.text)
                    b = a.img[txtoff + (o - txtaddr)];
                else if (o >= dataddr && o < dataddr + a.data)
                    b = a.img[datoff + (o - dataddr)];
                mem_w8(pa + i, b);
            }
        }
        printf("  loaded %s: text=%u@%08x data=%u@%08x bss=%u entry=%08x "
               "(%u pages unmapped)\n", path, a.text, txtaddr, a.data, dataddr,
               a.bss, a.entry, missing);
        if (missing) return 0;
    }

    /* Arguments on the process's own stack: strings, then the argv/envp arrays.
       Hand-loaded images additionally get argc in front, which is what crt0
       unpacks; the exec stub passes the arrays to execve instead. */
    u32 sp = stk_hi, sptr[MAX_UARGV + MAX_UENVP + 2];
    unsigned ns = 0;
    const char *dfl_av[1] = { "prog" };
    const char **av = nuargv ? (const char **)uargv : dfl_av;
    unsigned nav = nuargv ? nuargv : 1;
    for (unsigned k = 0; k < nav; k++) {
        u32 l = (u32)strlen(av[k]) + 1;
        sp -= l;
        for (u32 i = 0; i < l; i++) uput8(pmap, sp + i, (u8)av[k][i]);
        sptr[ns++] = sp;
    }
    for (unsigned k = 0; k < nuenvp; k++) {
        u32 l = (u32)strlen(uenvp[k]) + 1;
        sp -= l;
        for (u32 i = 0; i < l; i++) uput8(pmap, sp + i, (u8)uenvp[k][i]);
        sptr[ns++] = sp;
    }
    u32 plen = (u32)strlen(gpath) + 1;
    sp -= plen;
    u32 upath = sp;
    for (u32 i = 0; i < plen; i++) uput8(pmap, sp + i, (u8)gpath[i]);

    sp = (sp - (!procexec + nav + 1 + nuenvp + 1) * 4) & ~7u;
    u32 w = sp;
    if (!procexec) { uput32(pmap, w, nav); w += 4; }   /* argc, for crt0 */
    u32 uargvp = w;
    for (unsigned k = 0; k < nav; k++)     { uput32(pmap, w, sptr[k]); w += 4; }
    uput32(pmap, w, 0); w += 4;
    u32 uenvpp = w;
    for (unsigned k = 0; k < nuenvp; k++)  { uput32(pmap, w, sptr[nav + k]); w += 4; }
    uput32(pmap, w, 0);

    /* execve(path, argv, envp) -- syscall 59, number in r9, args in r2/r3/r4,
       trap 128; the kernel returns to pc+4 on error and pc+8 on success (which
       for a successful exec never happens -- it enters the new image instead). */
    u32 stub[] = {
        0x5C400000u | (upath  >> 16), 0x58420000u | (upath  & 0xFFFF), /* r2 */
        0x5C600000u | (uargvp >> 16), 0x58630000u | (uargvp & 0xFFFF), /* r3 */
        0x5C800000u | (uenvpp >> 16), 0x58840000u | (uenvpp & 0xFFFF), /* r4 */
        0x5920003Bu,                       /* or  r9, r0, 59  ; execve  */
        0xF000D080u,                       /* tb0 0, r0, 128            */
        0xC0000000u,                       /* +0x20 br . -- exec failed */
        0xC0000000u,                       /* +0x24 br . -- "succeeded" without
                                              replacing the image, which means
                                              the walker lost the new address
                                              space */
    };
    if (procexec) {
        for (unsigned i = 0; i < sizeof stub / 4; i++)
            uput32(pmap, ubase + i * 4, stub[i]);
        printf("  exec stub at VA 0: execve(\"%s\", argv@%08x, envp@%08x), sp=%08x\n",
               gpath, uargvp, uenvpp, sp);
    }

    /* --- make it the running process, the kernel's own way ---
       load_context(ctx) restores only r15..r31 and rte's to ctx+0x80, so the
       entry arguments cannot be passed in r2/r3.  Park them in r15/r16 (which
       ARE restored) and resume at a three-instruction trampoline that moves them
       into place and branches to _icode(entry, sp, 0) -- the kernel's own
       enter-user-mode gate (clears PSR bit31, sets SR1/SNIP/SFIP, rte). */
    u32 tramp = PROCEXP_TRAMP;                /* kernel VA hole: past text, below data */
    u32 tpa = translate(tramp, 0);
    if (mem_r32(tpa) || mem_r32(tpa + 4)) {
        printf("  !! trampoline page %08x is not free\n", tramp);
        return 0;
    }
    mem_w32(tpa + 0,  0x584F0000u);           /* or r2, r15, 0   ; entry */
    mem_w32(tpa + 4,  0x58700000u);           /* or r3, r16, 0   ; sp    */
    mem_w32(tpa + 8,  0x58800000u);           /* or r4, r0,  0   ; flag  */
    mem_w32(tpa + 12, 0xC0000000u |           /* br _icode              */
                      (((0xC0016E30u - (tramp + 12)) >> 2) & 0x03FFFFFFu));
    mem_w32(tpa + 0x20, 0xC0000000u);         /* br .  -- the idle sentinel */

    /* We are about to abandon proc0 mid-flight.  When the process exits, the
       kernel switches back to proc0 (its per-CPU idle thread) and load_contexts
       a context nobody ever saved -- which is why execution used to wander off
       into unmapped user VAs.  Snapshot proc0 properly with the kernel's own
       save_context (c00173ec: stores r1 as the resume PC, r14..r31, the PSR at
       +0x8c and the CPU number at +0xb4), then point its resume PC at the
       sentinel spin so the emulator can see the process finish. */
    u32 pctx = kr32(cur + P_CTX);
    kcall(0xC00173ECu, pctx, 0, 0, 0, 0);
    mem_w32(translate(pctx + 0x80, 0), PROCEXP_IDLE);
    printf("  proc0 context %08x saved; it will resume at the idle sentinel %08x\n",
           pctx, PROCEXP_IDLE);

    mem_w32(translate(ctx + 0x3c, 0), a.entry);   /* r15 = entry */
    mem_w32(translate(ctx + 0x40, 0), sp);        /* r16 = user sp */
    mem_w32(translate(ctx + 0x80, 0), tramp);     /* resume PC */
    mem_w32(translate(G_CURPROC, 0), p);          /* u.u_procp = the new proc */

    deliver_traps = 1;
    WR(2, ctx);
    cpu.pc = 0xC0017498u;                         /* load_context(ctx) */
    printf("  load_context(%08x): resuming as pid %u, entry %08x, sp %08x\n",
           ctx, kr32(p + P_PID) >> 16, a.entry, sp);
    return 1;
}

/* First real-exec experiment: build a fresh user address space with the kernel's
   own VM primitives, purely to prove the RPC path and that these functions run
   standalone at the idle point. */
void vm_experiment(void)
{
    printf("=== VM experiment: driving kernel VM functions via RPC ===\n");
    /* Reference: the kernel's currently-active supervisor APR (its segment-table
       root), so we can recognise a segment-table-shaped value in the pmap. */
    u32 sapr = mem_r32(0xFFF7E000u + CMMU_SAPR);
    printf("  (kernel SAPR = %08x)\n", sapr);

    /* Proven: the kernel's own VM allocators run standalone via RPC and return
       real structures.  This is the toolkit for building a real user address
       space; the remaining RE is the pmap->segment-table/APR relationship and
       the Mach VM call signatures (pmap_enter's 2nd arg is a pointer, not a raw
       VA). */
    u32 pmap = kcall(0xC0094058u, 0, 0, 0, 0, 0);           /* pmap_create(0)   */
    printf("  pmap_create(0)         = %08x\n", pmap);
    if (pmap < 0xC0000000u || pmap == 0xFFFFFFFFu) return;
    printf("  pmap struct:");
    for (u32 i = 0; i < 24; i++) {
        if (i % 6 == 0) printf("\n    +0x%02x:", i * 4);
        printf(" %08x", mem_r32(translate(pmap + i * 4, 0)));
    }
    printf("\n");

    u32 map = kcall(0xC004F9BCu, pmap, 0, 0x80000000u, 1, 0); /* vm_map_create  */
    printf("  vm_map_create(pmap)    = %08x\n", map);
}

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

/* Deliver an MC88100 ACCESS FAULT.  `vector` is the real MC88100 exception
   vector number: 2 = instruction access, 3 = data access.  The kernel's
   handlers are NOT where the symbol table says -- the whole _X* block from
   c0016180 on is shifted by one entry, so the routine labelled `_Xdataccess`
   (c0016180) is really _Xcodaccess and posts trap type 2, and the one labelled
   `_Xmisaligned` (c0016190) is really _Xdataccess and posts type 3.  The trap
   types the kernel switches on in `trap` (c00a9bd8) equal the vector numbers;
   its own name table at 0xC100D740 spells them out ("Instruction Access
   Exception" at 2, "Data Access Exception" at 3).

   The two vectors are resolved by completely different kernel paths:

     vector 2 (code) is handled in trap()'s case at c00a9ff0, which vm_faults
     the page containing SXIP.  Nothing else is needed from us.

     vector 3 (data) does NOTHING in trap() -- its case just returns.  A data
     fault is resolved earlier, in `exreturn` (c00aa3d4), called from the top of
     trap() before the switch.  exreturn walks the saved data pipeline: it
     gives up immediately unless DMT0 (frame+0x18) has its VALID bit set, takes
     the fault address from DMA0 (frame+0x20), the read/write protection from
     DMT0 & 0x1002, and calls vm_fault(map, addr & ~0x1fff, prot, 0, 0).  So a
     data fault MUST arrive with a valid data-pipeline transaction or the
     kernel has no idea what to page in.

   The faulting instruction has not completed, so we describe exactly one
   pending transaction and let the instruction re-execute on return (see the
   SXIP/SNIP note below).  The kernel's own software replay of the transaction
   (data_access_emulation, c00aaa44) is suppressed in step() -- our
   re-execution does the access instead, and letting both run would perform a
   partial-width store twice. */
void deliver_fault(u32 vector, u32 pc, u32 va, int code, int write, u32 width)
{
    u32 base = code ? 0xFFF7F000u : 0xFFF7E000u;
    mem_w32(base + CMMU_PFAR, va);
    /* PFSR fault code lives in bits 16-18; exreturn accepts 3-7 and routes
       4, 5 and 7 to vm_fault.  5 = page fault is what a missing page is. */
    mem_w32(base + CMMU_PFSR, 0x00050000u);
    cpu.cr[8] = cpu.cr[11] = cpu.cr[14] = 0;  /* DMT0/1/2: no transaction */
    if (!code) {
        /* DMT0: bit0 V, bit1 W, bits 2-5 byte enables, bits 7-11 destination
           register, bit 13 double, bit 14 xmem.  The byte-enable encoding is
           bit5 = the most significant byte lane down to bit2 = the least, as
           data_access_emulation (c00aaa44) decodes it: 0x3c word, 0x30/0x0c
           the two halfwords, 0x20/0x10/0x08/0x04 the four bytes.  It is only
           ever read for the replay we suppress, but an accurate description
           costs nothing and keeps the machine state coherent.  Destination
           register 0: trapcommon restores r1..r31 from frame+0x40 and never
           reads the r0 slot, so nothing can come of it. */
        u32 benable;
        switch (width) {
        case 1:  benable = 0x20u >> (va & 3u); break;
        case 2:  benable = (va & 2u) ? 0x0Cu : 0x30u; break;
        default: benable = 0x3Cu; break;      /* 4, and 8 as its first half   */
        }
        cpu.cr[8]  = 1u | (write ? 2u : 0u) | benable;
        cpu.cr[9]  = 0;                       /* DMD0: store data NOT modelled */
        cpu.cr[10] = va;                      /* DMA0: the faulting address   */
    }
    cpu.cr[2] = cpu.cr[1];                    /* EPSR = PSR at fault time     */
    /* SXIP must be INVALID: saveregs (c00167e8) clears SNIP, points SFIP at its
       continuation and rte's, relying on the first VALID pipeline register
       being that continuation.  Leaving SXIP valid makes its rte jump straight
       back to the faulting instruction and the handler is never reached.  The
       resume point therefore lives in SNIP -- pointing at the faulting
       instruction itself, so the eventual return re-executes it. */
    cpu.cr[4] = pc;                           /* SXIP = faulting insn, INVALID */
    cpu.cr[5] = pc | 2u;                      /* SNIP = resume here, VALID     */
    cpu.cr[6] = (pc + 4u) | 2u;               /* SFIP                          */
    cpu.cr[1] = cpu.cr[1] | 0x80000003u;      /* supervisor | IND | SFRZ      */
    cpu.pc = cpu.cr[7] + vector * 8u;
    cpu.has_pending = 0;
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
    if (vmprobe) vm_probe_tick(pc);
    if (ctxtrace && pc == 0xC0017498u) ctx_tick();     /* load_context(ctx) */
    if (stwatch_pc) stwatch_tick(pc);
    if (regfind_val) regfind_tick(pc);

    /* --hwfault: skip the kernel's software replay of the aborted data
       transaction (data_access_emulation, c00aaa44, which exreturn calls once
       vm_fault has made the page present).  Our fault model aborts the
       faulting instruction before any register or byte of memory is touched
       and re-executes it on return, so the access already happens exactly
       once.  The replay reads its store data from DMD0, which we do not model
       -- the very first fault of every exec is copyout's `st.b r5,[r3+r0]`
       tail at c00171e4, and a replay of DMD0 = 0 would write a zero byte over
       the argv string before the re-execution put the right one back.  It
       would also turn a faulting xmem into an exchange performed twice.
       Return 0 in r2; the caller ignores the value. */
    if (hwfault && sysmode && pc == 0xC00AAA44u) {
        cpu.pc = RD(1);
        WR(2, 0);
        return 0;
    }

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
            /* Route by device: the root device (first seen, the tape) reads from
               root_img; any other block device (the SCSI disk) uses disk.img. */
            u32 vp = mem_r32(translate(buf + 0x50, 0));
            if (!root_dev_vp) root_dev_vp = vp;
            int is_root = (vp == root_dev_vp);
            FILE *dev = is_root ? root_img : disk_img;
            if (dev && addr >= 0xC0000000u && bcount && bcount <= 0x10000u) {
                u8 tmp[0x10000];
                if (flags & 1u) {                       /* read */
                    if (fseek(dev, (long)blkno * 512, SEEK_SET) == 0) {
                        size_t got = fread(tmp, 1, bcount, dev);
                        for (size_t k = got; k < bcount; k++) tmp[k] = 0;
                        for (u32 k = 0; k < bcount; k += 4)
                            mem_w32(translate(addr + k, 0), be32(tmp + k));
                    }
                } else if (!is_root) {                  /* write-back to disk */
                    for (u32 k = 0; k < bcount; k += 4) {
                        u32 w = mem_r32(translate(addr + k, 0));
                        tmp[k]=w>>24; tmp[k+1]=w>>16; tmp[k+2]=w>>8; tmp[k+3]=w;
                    }
                    if (fseek(dev, (long)blkno * 512, SEEK_SET) == 0)
                        fwrite(tmp, 1, bcount, dev), fflush(dev);
                }
            }
            if (scsi_trace)
                printf("[%s-%s] buf=%08x flags=%08x blk=%u bcount=%u addr=%08x @%llu\n",
                       pc == SDSTRATEGY ? "strategy" : "getblk",
                       is_root ? "root" : "sd0", buf, flags, blkno, bcount, addr,
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
            /* Route by device (buf +0x50): root device -> tape (read-only);
               the SCSI disk -> disk.img (read AND write-back). */
            u32 vp = mem_r32(translate(buf + 0x50, 0));
            if (!root_dev_vp) root_dev_vp = vp;
            int is_root = (vp == root_dev_vp);
            FILE *dev = is_root ? root_img : disk_img;
            if (dev && addr >= 0xC0000000u && bcount && bcount <= 0x10000u) {
                u8 tmp[0x10000];
                if (is_read) {
                    if (fseek(dev, (long)blkno * 512, SEEK_SET) == 0) {
                        size_t got = fread(tmp, 1, bcount, dev);
                        for (size_t k = got; k < bcount; k++) tmp[k] = 0;
                        for (u32 k = 0; k < bcount; k += 4)
                            mem_w32(translate(addr + k, 0), be32(tmp + k));
                    }
                } else if (!is_root) {              /* write-back to the disk */
                    for (u32 k = 0; k < bcount; k += 4) {
                        u32 w = mem_r32(translate(addr + k, 0));
                        tmp[k]=w>>24; tmp[k+1]=w>>16; tmp[k+2]=w>>8; tmp[k+3]=w;
                    }
                    if (fseek(dev, (long)blkno * 512, SEEK_SET) == 0)
                        fwrite(tmp, 1, bcount, dev), fflush(dev);
                }
                if (scsi_trace)
                    printf("[bio-%s] %s blk=%u bcount=%u addr=%08x\n",
                           is_read ? "rd" : "wr", is_root ? "root" : "sd0",
                           blkno, bcount, addr);
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

    if (watch_pc && pc == watch_pc) {
        printf("[watch] pc=%08x r1=%08x r2=%08x r3=%08x r4=%08x r5=%08x r6=%08x "
               "| r23=%08x r24=%08x r25=%08x r26=%08x r27=%08x @%llu\n",
               pc, RD(1), RD(2), RD(3), RD(4), RD(5), RD(6),
               RD(23), RD(24), RD(25), RD(26), RD(27),
               (unsigned long long)cpu.count);
    }

    u32 w   = mem_r32(translate(pc, 1));
    /* Instruction fetch faulted: there is no instruction to run.  Bail before
       decoding -- otherwise the garbage word gets executed, and its operands
       (a load from VA 0, typically) mask the real fault. */
    if (ufault_pending) return 0;
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

    /* A user page fault aborts the instruction: memop bailed before touching
       memory or a register, so leaving cpu.pc alone re-executes it once the
       mapping exists.  Nothing else in this step is observable. */
    if (ufault_pending) return 0;

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
