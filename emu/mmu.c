/* Generated split of the former single-file nx88.c.
   Shared types, macros, hot inline accessors, and all
   cross-module declarations live in nx88.h. */
#include "nx88.h"

/* Map [lo,hi) with PTE physical = va - off.  off=0 gives identity (device
   windows); off=0xC0000000 gives the kernel's direct map (VA 0xC0000000->PA 0),
   which is what --realmm needs so the kernel's own PTE writes land at real
   node-0 physical addresses. */
void map_range_off(u32 segtab, u32 lo, u32 hi, u32 off)
{
    for (u32 va = lo; va < hi; va += PAGE_SIZE) {
        u32 sidx = (va >> 22) & 0x3FF;
        u32 sdesc = mem_r32(segtab + sidx * 4);
        u32 pgtab;
        if (sdesc & 1) {
            pgtab = sdesc & 0xFFFFF000u;
        } else {
            pgtab = ptpool_next;
            ptpool_next += PAGE_SIZE;
            mem_zero(pgtab, PAGE_SIZE);
            mem_w32(segtab + sidx * 4, pgtab | 1);   /* valid */
        }
        mem_w32(pgtab + ((va >> 12) & 0x3FF) * 4, ((va - off) & 0xFFFFF000u) | 1);
    }
}

void map_range(u32 segtab, u32 lo, u32 hi) { map_range_off(segtab, lo, hi, 0); }

void build_free_list(void)
{
    u32 head = mem_r32(0xC0014038u);
    if (!head) { head = 0x40000000u; mem_w32(0xC0014038u, head); }
    u32 n = 0x60;                        /* limit at 0xC100DC58 */
    for (u32 i = 0; i < n; i++) {
        u32 slot = head + i * fl_stride;
        /* the free list lives in the interleaver pool, reached through the
           CMRAM data port -- populate the dedicated CMRAM store, not main RAM */
        cmram_w32(slot, (i + 1 < n) ? head + (i + 1) * fl_stride : 0);
    }
    printf("synthetic free list: head %08x, %u entries, stride %#x\n",
           head, n, fl_stride);
}

void boot_build_tables(void)
{
    mem_zero(CODE_SEGTAB, PAGE_SIZE);
    mem_zero(DATA_SEGTAB, PAGE_SIZE);

    /* device windows are identity-mapped (fixed physical); kernel RAM uses the
       direct map (VA-KOFF) under --realmm, identity otherwise */
    static const u32 devs[][2] = {
        { 0xE0700000u, 0xE0800000u },     /* DUART, interleaver, node ctrl */
        { 0xFF040000u, 0xFF050000u },     /* mmu tables / stack area       */
        { 0xFFF70000u, 0xFFF80000u },     /* CMMU register windows         */
    };
    u32 koff = realmm ? KOFF : 0;
    /* kernel space: 0xC0000000-0xE0000000 -> PA 0-0x20000000 (realmm) or identity */
    u32 dsplit = kdata_off ? kdata_va : 0xE0000000u;
    map_range_off(CODE_SEGTAB, 0xC0000000u, dsplit, koff);
    map_range_off(DATA_SEGTAB, 0xC0000000u, dsplit, koff);
    if (kdata_off) {        /* data/bss and the heap above it sit lower */
        map_range_off(CODE_SEGTAB, dsplit, 0xE0000000u, kdata_off);
        map_range_off(DATA_SEGTAB, dsplit, 0xE0000000u, kdata_off);
    }
    for (unsigned i = 0; i < sizeof devs / sizeof devs[0]; i++) {
        map_range(CODE_SEGTAB, devs[i][0], devs[i][1]);
        map_range(DATA_SEGTAB, devs[i][0], devs[i][1]);
    }

    /* pre-load the APRs exactly as the kernel will later write them */
    for (int i = 0; i < n_cmmu; i++) {
        u32 tab = (cmmu_present[i] == 0xFFF7F000u) ? CODE_SEGTAB : DATA_SEGTAB;
        mem_w32(cmmu_present[i] + CMMU_SAPR, tab | 1);
        mem_w32(cmmu_present[i] + CMMU_UAPR, DATA_SEGTAB | 1);
    }
    printf("synthetic boot: %s map, tables at %08x (code) / %08x (data), "
           "%u page tables\n", realmm ? "direct(VA-0xC0000000)" : "identity",
           CODE_SEGTAB, DATA_SEGTAB, (ptpool_next - PTPOOL) / PAGE_SIZE);
}

/* The kernel builds its page tables through kernel VIRTUAL addresses but names
   them physically in the APR, and this emulator does not place kernel memory at
   the physical addresses the kernel believes in -- so a descriptor read at the
   APR's address finds nothing.  ktab_bias (emulator PA - kernel PA, derived from
   any pmap as (pmap[0]-KOFF) - pmap[1]) reaches the same table where it really
   lives.  Zero when unused, so the default boot is untouched.

   Scope matters: this is applied ONLY to user-space VAs.  Kernel data still
   resolves through the synthetic direct map, which is where its contents
   actually are -- biasing kernel VAs too silently sends 500+ accesses per
   syscall to the kernel's *intended* physical pages, which in this emulator are
   empty.  Kernel-space coherence is the rest of the realmm work. */
static u32 tab_r32(u32 pa, int biased)
{
    u32 v = mem_r32(pa);
    if (!(v & 1) && biased) {
        u32 alt = mem_r32((pa & 0x3FFFFFFFu) + ktab_bias);
        if (alt & 1) return alt;
    }
    return v;
}

/* Two-level MC88200 walk: area pointer -> segment table -> page table. */
int mmu_walk(u32 apr, u32 vaddr, u32 *phys)
{
    u32 segtab = apr & 0xFFFFF000u;
    if (!segtab) return 0;
    if (ileave_stub && !realmm && (segtab & 0xC0000000u) == 0x80000000u &&
        !mem_r32(segtab + ((vaddr >> 22) & 0x3FF) * 4)) {
        segtab = DATA_SEGTAB;
        ileave_redirects++;
    }
    int biased = ktab_bias && vaddr < 0xC0000000u;
    u32 sdesc = tab_r32(segtab + ((vaddr >> 22) & 0x3FF) * 4, biased);
    if (!(sdesc & 1)) return 0;                       /* segment invalid */
    u32 pgtab = sdesc & 0xFFFFF000u;
    u32 pdesc = tab_r32(pgtab + ((vaddr >> 12) & 0x3FF) * 4, biased);
    if (!(pdesc & 1)) return 0;                       /* page invalid */
    *phys = (pdesc & 0xFFFFF000u) | (vaddr & 0xFFF);
    if (biased) kwalk_user++;
    return 1;
}

void devmap_add(u32 va, u32 pa)
{
    va &= ~0xFFFu; pa &= ~0xFFFu;
    for (int i = 0; i < ndevmap; i++)
        if (devmap[i].va == va) { devmap[i].pa = pa; return; }
    if (ndevmap < (int)(sizeof devmap / sizeof devmap[0])) {
        devmap[ndevmap].va = va;
        devmap[ndevmap].pa = pa;
        ndevmap++;
    }
}

u32 devmap_lookup(u32 va)
{
    va &= ~0xFFFu;
    for (int i = 0; i < ndevmap; i++)
        if (devmap[i].va == va) return devmap[i].pa;
    return 0;
}

/* Walk the kernel's active table, then (realmm) fall back to the synthetic
   direct-map table so the kernel's own vtop and the CPU's translate() agree. */
int walk_fb(u32 apr, u32 vaddr, int code, u32 *phys)
{
    if (mmu_walk(apr, vaddr, phys)) return 1;
    if (realmm && ndevmap) {
        u32 dp = devmap_lookup(vaddr);
        if (dp) { *phys = dp | (vaddr & 0xFFF); return 1; }
    }
    /* Fall back to the synthetic table on ANY miss (segment- or page-level) in
       the kernel's own table.  The kernel's table is only partially populated
       in our model, so a present segment descriptor with a missing page entry
       would otherwise fail even though the address is really mapped.  Applies
       to both realmm (direct map) and the identity path (--ileave). */
    if (realmm || ileave_stub) {
        u32 syn = code ? CODE_SEGTAB : DATA_SEGTAB;
        if (mmu_walk(syn | 1, vaddr, phys)) return 1;
        if (vaddr >= 0xE0000000u) { *phys = vaddr; return 1; }
    }
    return 0;
}

void cmmu_command(u32 base, u32 cmd)
{
    /* 0x20/0x24 are the user/supervisor address-probe commands; everything else
       is a cache/TLB invalidate.  Those used to be ignorable because kernel VAs
       bypassed the TLB entirely (a hardcoded direct map), but once kernel space
       is resolved through the kernel's own page tables a stale entry survives a
       PTE change and the kernel reads memory that has moved.  Flush
       conservatively -- correctness first; the TLB refills immediately. */
    if (cmd != 0x20 && cmd != 0x24) { tlb_flush(); return; }
    u32 vaddr = mem_r32(base + CMMU_SAR);
    u32 apr   = mem_r32(base + (cmd == 0x24 ? CMMU_SAPR : CMMU_UAPR));
    int is_code = (base == 0xFFF7F000u);
    u32 phys  = 0;
    if (walk_fb(apr, vaddr, is_code, &phys)) {
        /* Single-node machine: all RAM is on the master node (node 0).  The
           kernel derives a page's node from vtop bits 28-23 (_m_expand); our
           reported PA for kernel memory carries phantom non-zero node bits
           (>8MB in decodes to node 2+), failing the kernel's "pool must be on
           master node" checks.  Force node 0 for kernel-space vtops so it tells
           the truth for a 1-node system.  Keyed on the input VA (kernel space)
           so it works for both identity and realmm (whose PAs are low). */
        if (vaddr >= 0xC0000000u && vaddr < 0xE0000000u)
            phys &= ~0x1F800000u;
        mem_w32(base + CMMU_SSR, 1);                  /* bit 0 = valid */
        mem_w32(base + CMMU_SAR, phys);
        probe_hits++;
    } else {
        mem_w32(base + CMMU_SSR, 0);
        mem_w32(base + CMMU_PFAR, vaddr);
        probe_misses++;
    }
}

void tlb_flush(void)
{
    memset(tlb, 0, sizeof tlb);
}

u32 translate(u32 va, int code)
{
    if (!translate_on) return va;

    /* realmm: kernel space (>=0xC0000000) is a GLOBAL direct map, identical in
       every address space.  Resolving it independently of the active APR is
       essential -- a function that changes the APR mid-execution (e.g. MMU
       setup routines) must still find its own stack unchanged.  Device pages
       the kernel mapped into kernel VA go through devmap; the rest is the fixed
       VA-KOFF direct map; >=0xE0000000 is identity device space. */
    if (realmm && va >= 0xC0000000u) {
        if (va >= 0xE0000000u) return va;
        u32 dp = devmap_lookup(va & ~0xFFFu);
        if (dp) return dp | (va & 0xFFF);
        /* --dataphys: the data segment is loaded at the physical address nX
           itself believes in -- right after text, not VA-KOFF.  Once that holds,
           the kernel's OWN page tables are readable at the physical addresses
           its APR names, so they become the truth for kernel space too; the
           linear map is only the bootstrap fallback for VAs they do not cover
           yet.  Without this, pages the kernel allocates from its physical page
           allocator (whose PAs follow no linear rule) are unreachable and it
           panics in its own vtop.  Cached in the TLB -- a walk on every kernel
           access would cost far too much. */
        if (kdata_off) {
            u32 lin = va >= kdata_va ? va - kdata_off : va - KOFF;
            u32 vpn = va >> 12, idx = vpn & (TLB_SIZE - 1);
            if (tlb[code][idx].tag == (vpn | 0x80000000u))
                return tlb[code][idx].pa | (va & 0xFFF);
            u32 apr = mem_r32((code ? 0xFFF7F000u : 0xFFF7E000u) + CMMU_SAPR);
            u32 pa;
            if (!(apr & 1) || !mmu_walk(apr, va, &pa)) {
                /* The kernel's table does not cover this VA, so we have to guess
                   with the linear map -- and a VA that later BECOMES covered is
                   then read from somewhere else than it was written.  That is
                   the corruption; count it. */
                if (kfall_n++ < 10)
                    printf("[kfall] kernel VA %08x not in the kernel's table, "
                           "using linear %08x  pc=%08x @%llu\n",
                           va, lin, cpu.pc, (unsigned long long)cpu.count);
                return lin;
            }
            if (pa != lin) kdis_n++;
            tlb[code][idx].tag = vpn | 0x80000000u;
            tlb[code][idx].pa  = pa & 0xFFFFF000u;
            return pa;
        }
        return va - KOFF;
    }

    /* Below kernel space: user address.  Supervisor accesses use SAPR (the
       kernel's map); user-mode accesses (PSR bit31 clear) use UAPR (the current
       process's map).  The kernel boots entirely in supervisor mode, so this
       only diverges once we drop a process to user mode. */
    u32 aproff = (cpu.cr[1] & 0x80000000u) ? CMMU_SAPR : CMMU_UAPR;
    u32 apr = mem_r32((code ? 0xFFF7F000u : 0xFFF7E000u) + aproff);
    if (!(apr & 1)) return va;                 /* translation disabled */

    u32 vpn = va >> 12;
    u32 idx = vpn & (TLB_SIZE - 1);
    if (tlb[code][idx].tag == (vpn | 0x80000000u))
        return tlb[code][idx].pa | (va & 0xFFF);

    /* walk_fb: kernel's table, then (realmm) the synthetic direct-map table
       which supplies node-correct physical addresses for kernel memory */
    u32 pa;
    if (!walk_fb(apr, va, code, &pa)) {
        /* Demand paging for our synthetic user space.  The kernel's VM has no
           idea this address space exists, so a real vector-2 fault would find
           no vm_map entry; instead we do what the pager would: hand out a
           zeroed page and map it.  This is what lets obreak's heap growth and
           deep stacks actually be touchable. */
        u32 dp = udemand_page(apr, va);
        if (dp) return dp | (va & 0xFFF);
        /* A real user process (--procexp) faults: the kernel's exec maps its
           image demand-paged, so the first touch of every page misses here.
           Record it and abort the instruction; run_sys resolves it through the
           kernel's own paging path and re-executes.  (A faithful MC88100 would
           deliver vector 1/2 to _Xcodaccess/_Xdataccess instead -- same hook,
           see the note in run_sys.) */
        if (procexp && !ufault_pending) {
            ufault_pending = 1;
            ufault_va = va;
            ufault_code = code;
            ufault_pc = cpu.pc;
            return 0;
        }
        xlat_faults++;
        last_fault_va = va;
        last_fault_pc = cpu.pc;
        if (realmm && xlat_faults <= 20)
            printf("[xlat MISS] va=%08x pc=%08x apr=%08x @%llu\n",
                   va, cpu.pc, apr, (unsigned long long)cpu.count);
        return realmm ? cphys(va) : va;
    }
    tlb[code][idx].tag = vpn | 0x80000000u;
    tlb[code][idx].pa  = pa & 0xFFFFF000u;
    return pa;
}
