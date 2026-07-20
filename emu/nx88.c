/*
 * nx88 -- MC88100 emulator for BBN nX (TC2000).
 *
 * C port of the Python core in ../tools.  Semantics are deliberately identical
 * so the two can be cross-checked against each other; the Python remains the
 * readable reference, this is the one that runs kernel bring-up at a usable
 * speed.
 *
 * The encoding facts this implements were recovered empirically from the
 * install-tape kernel, not from documentation:
 *
 *   - 5 instruction formats selected by bits 31-26.
 *   - Triadic (0x3D): opcode in bits 15-10 mirroring the immediate map,
 *     variant in bits 9-5 (0x10 = scaled index), S2 in bits 4-0.
 *   - Immediate `and`/`or`/`xor` affect only ONE halfword and leave the other
 *     intact; `mask` zeroes the other half.  This is what makes the compiler's
 *     `and.u rX,rX,0x7f` + `and rX,rX,0x8000` pair compose to 0x007F8000.
 *   - `cmp` deposits a condition bit-vector, tested later by `bb0`/`bb1`.
 *   - Delay slots are explicit: only .n-suffixed branches execute the
 *     following instruction before transferring control.
 *   - Syscalls: number in r9, args r2.., trap `tb0 0,r0,128`, and the kernel
 *     returns to pc+4 on ERROR but pc+8 on SUCCESS (it skips an error branch).
 *
 * Build:  gcc -O2 -o nx88 nx88.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <fcntl.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int32_t  s32;
typedef uint64_t u64;

/* ------------------------------------------------------------------ memory */

#define PAGE_SIZE 4096
#define NPAGES    (1u << 20)          /* 4 GiB of address space, sparse */

static u8 **pages;

/*
 * Real-memory model (--realmm): the kernel runs at true physical addresses.
 * On a single node the switch's interleaved view (mode bit 0x80000000) and the
 * node-local view alias the same RAM, so interleaved physical addresses
 * 0x80000000-0xBFFFFFFF fold onto node-local 0x00000000-0x3FFFFFFF.  Doing this
 * in page_of() -- the one backing-store choke point -- keeps CPU accesses,
 * page-table walks, and the CMRAM window automatically coherent: a PTE written
 * to node-local 0x110000 (via the kernel's direct map) is the same byte the
 * walk reads when the APR points at interleaved 0x80110000.
 */
static int realmm;
static int dbg_trans;

static inline u32 cphys(u32 a)
{
    if (realmm && a >= 0x80000000u && a < 0xC0000000u)
        return a & 0x7FFFFFFFu;
    return a;
}

static inline u8 *page_of(u32 a)
{
    u32 i = cphys(a) >> 12;
    u8 *p = pages[i];
    if (!p) { p = calloc(1, PAGE_SIZE); pages[i] = p; }
    return p;
}

static inline u8 mem_r8(u32 a) { return page_of(a)[a & 4095]; }
static inline void mem_w8(u32 a, u8 v) { page_of(a)[a & 4095] = v; }

static u32 mem_r32(u32 a)
{
    if ((a & 4095) <= 4092) {
        u8 *p = page_of(a) + (a & 4095);
        return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
    }
    return ((u32)mem_r8(a) << 24) | ((u32)mem_r8(a+1) << 16) |
           ((u32)mem_r8(a+2) << 8) | mem_r8(a+3);
}

static u32 wmem_addr;                 /* --wmem=ADDR: trace writes to a word */
static u32 wmem_lo, wmem_hi;          /* --wrange=LO:HI: trace writes in range */
static u32 dbg_pc;                    /* current pc, for trace prints */
static u64 dbg_count;                 /* current instruction count */

static void mem_w32(u32 a, u32 v)
{
    if (wmem_addr && a == wmem_addr)
        printf("[wmem] %08x <- %08x  pc=%08x @%llu\n", a, v, dbg_pc,
               (unsigned long long)dbg_count);
    if (wmem_hi && a >= wmem_lo && a < wmem_hi)
        printf("[wrange] %08x <- %08x  pc=%08x @%llu\n", a, v, dbg_pc,
               (unsigned long long)dbg_count);
    if ((a & 4095) <= 4092) {
        u8 *p = page_of(a) + (a & 4095);
        p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
        return;
    }
    mem_w8(a, v >> 24); mem_w8(a+1, v >> 16);
    mem_w8(a+2, v >> 8); mem_w8(a+3, v);
}

static u16 mem_r16(u32 a) { return ((u16)mem_r8(a) << 8) | mem_r8(a+1); }
static void mem_w16(u32 a, u16 v) { mem_w8(a, v >> 8); mem_w8(a+1, v); }

static void mem_load(u32 a, const u8 *src, size_t n)
{
    for (size_t i = 0; i < n; i++) mem_w8(a + (u32)i, src[i]);
}

static void mem_zero(u32 a, size_t n)
{
    for (size_t i = 0; i < n; i++) mem_w8(a + (u32)i, 0);
}

static int mem_cstr(u32 a, char *out, int max)
{
    int i = 0;
    while (i < max - 1) { u8 c = mem_r8(a + i); if (!c) break; out[i++] = (char)c; }
    out[i] = 0;
    return i;
}

/* ------------------------------------------------------------------- cpu */

typedef struct {
    u32 r[32];
    u32 cr[64];
    u32 pc;
    u32 pending;          /* delay-slot branch target */
    int has_pending;
    u64 count;
} CPU;

static CPU cpu;

/* trap reporting */
static int   trap_taken;
static u32   trap_vector;
static u32   trap_pc;

#define RD(n)      ((n) ? cpu.r[n] : 0u)
#define WR(n, v)   do { if (n) cpu.r[n] = (u32)(v); } while (0)

/* cmp condition bit positions */
enum { C_EQ = 2, C_NE = 3, C_GT = 4, C_LE = 5, C_LT = 6,
       C_GE = 7, C_HI = 8, C_LS = 9, C_LO = 10, C_HS = 11 };

static u32 do_cmp(u32 a, u32 b)
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

static int cond_true(u32 m5, u32 a)
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

static void bitfield(u32 sub, u32 D, u32 a, u32 width, u32 offset)
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

static inline u32 dev_read32(u32 a);    /* device-aware load  (timer, CMMU IDs) */
static inline void dev_write32(u32 a, u32 v); /* device-aware store (CMMU commands) */
static inline void tcs_poke(u32 a);           /* TCS mailbox handshake */

/* memory-op helper shared by the immediate and triadic forms */
static inline u32 translate(u32 va, int code);

static void memop(u32 sub, u32 D, u32 ea)
{
    /* lda computes an effective ADDRESS only -- it does not access memory, so
       it must return the untranslated virtual address.  Everything else
       translates before touching memory. */
    if (sub >= 0x0C && sub <= 0x0F) { WR(D, ea); return; }
    ea = translate(ea, 0);
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

static const u8 memop_scale[16] = {
    1,4, 2,1, 8,4,2,1, 8,4,2,1, 8,4,2,1
};

/* system-mode device hooks (set by the sysmode driver) */
static int  sysmode;
static u32  tick_scale = 2000;
static u32  force_sig_pc;      /* pc at which to force the TCS EEPROM sig */
static u32  force_sig_val;
static int  log_msgs;
#define LOG_ROUTINE     0xC005A85Cu
#define PRINTF_THROTTLE 0xC005A9D0u

#define TIMER_ADDR 0xE07E8018u

/*
 * MC88200 CMMU ID registers.
 *
 * The kernel probes for CMMUs by scanning n = 0..0xFF, reading the word at
 * 0xFFF00000 + (n << 12), and requiring
 *
 *     (id & 0xFFE00000) == (((n << 24) | 0x00A00000) & 0xFFE00000)
 *
 * i.e. each CMMU reports an ID that encodes its own position.  That single
 * rule reproduces all three hard-coded checks in the boot path: the code MMU
 * at 0xFFF7F000 wants 0x7FA00000, and the two others at 0xFFF7D000 /
 * 0xFFF7E000 want 0x7DA00000 / 0x7EA00000.  Getting this wrong panics with
 * "invalid code_mmu" before the kernel does anything else.
 */
#define CMMU_BASE 0xFFF00000u

/*
 * Which CMMUs this node actually has.  Advertising one at every slot is wrong:
 * the boot path cross-checks the second code MMU against a per-node config
 * byte at 0xC00156B5, and panics "invalid code_mmu2" if hardware and config
 * disagree.  A minimal node is one code MMU plus one data MMU, which matches
 * that byte reading zero.
 */
static u32 cmmu_present[8] = { 0xFFF7F000u, 0xFFF7E000u };
static int n_cmmu = 2;

static inline int is_cmmu_id(u32 a)
{
    for (int i = 0; i < n_cmmu; i++) if (cmmu_present[i] == a) return 1;
    return 0;
}

static inline u32 cmmu_id_for(u32 a)
{
    u32 n = (a >> 12) & 0xFF;
    return (n << 24) | 0x00A00000u;
}

/*
 * CMRAM window -- the Butterfly switch interleaver's translation RAM.
 *
 * Accessed through a select/data port pair rather than being directly
 * addressable (confirmed from _cmram_interleave_setup, _interleaver_pool_rw,
 * and _paddr2smram):
 *
 *     0xE07EA00C  select   -- latches which CMRAM slot the data port refers to
 *     0xE07EB000  data     -- read/write the selected slot
 *     0xE07EA008/010/028   -- status/ack (return "ready")
 *
 * Faithfully scattering interleaved (0x8xxxxxxx) memory through these
 * descriptors is the full switch model and still to do; what this provides is
 * coherent RAM *semantics* for the window, so a value written to a slot reads
 * back from it.  That alone stops the master-mapper free list from being
 * corrupted to zero (the bug that panicked "out of master mapper ram"): the
 * data port was returning 0, so _paddr2smram wrote 0 back over the free-list
 * head.
 */
#define CMRAM_SELECT 0xE07EA00Cu
#define CMRAM_DATA   0xE07EB000u

/*
 * The select value is a full 32-bit interleaved address (e.g. 0x40000000 for
 * the master-mapper free list, 0x82004001 for descriptor slots), so the window
 * is keyed by the whole select and proxied to backing memory.  Reading or
 * writing the data port therefore hits the same storage a direct access to
 * that interleaved address would -- window and direct views stay coherent.
 */
static u32 cmram_sel;
static u64 cmram_reads, cmram_writes;

static inline u32 dev_read32(u32 a)
{
    if (sysmode) {
        if (a == TIMER_ADDR) return (u32)(cpu.count * tick_scale);
        if (is_cmmu_id(a))   return cmmu_id_for(a);
        if (a == CMRAM_DATA) { cmram_reads++; return mem_r32(cmram_sel & ~3u); }
        if (a == 0xE07EA008u || a == 0xE07EA010u || a == 0xE07EA028u)
            return 0;                    /* status ports: ready, no error */
    }
    return mem_r32(a);
}

/*
 * MC88200 register offsets, as used by the kernel's own probe routine
 * (_full_svtop):
 *
 *    +0x004  SCR   system command   (0x24 = probe supervisor address)
 *    +0x008  SSR   system status    (bit 0 = translation valid)
 *    +0x00C  SAR   search address   (written with the VA, read back as PA)
 *    +0x108  PFAR  page fault address
 *    +0x200  SAPR  supervisor area pointer
 *    +0x204  UAPR  user area pointer
 *    +0x400  BWP0..7 batc write ports
 *
 * Everything except the command register behaves as ordinary memory, so only
 * the SCR write needs intercepting: it runs the two-level table walk and
 * deposits the answer where the kernel expects to read it.
 */
#define CMMU_SCR  0x004
#define CMMU_SSR  0x008
#define CMMU_SAR  0x00C
#define CMMU_PFAR 0x108
#define CMMU_SAPR 0x200
#define CMMU_UAPR 0x204

static int  mmu_trace;
static int  batc_trace;
static u32  dump_addr;
static u32  findpt_va;
static u32  watch_pc;
static u64  probe_hits, probe_misses;

/*
 * Synthetic boot loader.
 *
 * The tape has no bootstrap: primary boot lives in the node's C108 PROMs and
 * the secondary loader (/stand/secboot) ships on the main distribution, not
 * the installer.  So the kernel is entered expecting page tables that nobody
 * built.
 *
 * Physical address bits 31-30 select a mode: 3 (0xC.../0xE.../0xF...) is
 * node-local, 2 (0x8...) is interleaved/global.  The kernel is linked at
 * 0xC0010000, which is already a valid node-local physical address -- so the
 * mapping a boot loader would install here is simply IDENTITY.  That is what
 * this builds, for the ranges the kernel actually touches.
 *
 * Segment table base addresses match what the kernel itself later writes to
 * the APRs (0xE0790000 code, 0x803F0000 data), so its own setup is a no-op.
 */
#define CODE_SEGTAB 0xE0790000u

#define DATA_SEGTAB 0x803F0000u
#define PTPOOL      0xDF000000u          /* above the kernel's memory, so the
                                            kernel's own dynamic allocations can
                                            never overwrite our page tables */

static int synth_boot = 1;
static u32 ptpool_next = PTPOOL;
static u32 seed_mapper;

/* Map [lo,hi) with PTE physical = va - off.  off=0 gives identity (device
   windows); off=0xC0000000 gives the kernel's direct map (VA 0xC0000000->PA 0),
   which is what --realmm needs so the kernel's own PTE writes land at real
   node-0 physical addresses. */
static void map_range_off(u32 segtab, u32 lo, u32 hi, u32 off)
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

static void map_range(u32 segtab, u32 lo, u32 hi) { map_range_off(segtab, lo, hi, 0); }

/*
 * Synthesise the master-mapper free list that the b2vme PROM would have built.
 * Head is the global at 0xC0014038 (=0x40000000); each node holds the address
 * of the next free slot; the last holds 0.  Stride is under test (--flstride),
 * because the mapper geometry isn't yet pinned: _vmeaddr2paddr reads the slot
 * from VME bits 30-25 (32MB granularity) yet the entry limit is 96 (>64), so
 * the two readings disagree and the correct stride has to be found by
 * observing the kernel's own use.
 */
/*
 * Stride defaults to 8KB (page-granular, no 32-bit overflow across 96 slots).
 * The mapper geometry isn't fully pinned, but the value is not yet exercised:
 * every stride tried reaches the same next blocker, so the free list is used
 * only as a pop-able chain here, not yet as live DMA addresses.  Revisit when
 * the SCSI controller actually DMAs through a mapped window.
 */
static u32 fl_stride = 0x2000;

static void build_free_list(void)
{
    u32 head = mem_r32(0xC0014038u);
    if (!head) { head = 0x40000000u; mem_w32(0xC0014038u, head); }
    u32 n = 0x60;                        /* limit at 0xC100DC58 */
    for (u32 i = 0; i < n; i++) {
        u32 slot = head + i * fl_stride;
        mem_w32(slot, (i + 1 < n) ? head + (i + 1) * fl_stride : 0);
    }
    printf("synthetic free list: head %08x, %u entries, stride %#x\n",
           head, n, fl_stride);
}

#define KOFF 0xC0000000u                  /* kernel VA -> PA offset (VA 0xC0000000 -> PA 0) */

static void boot_build_tables(void)
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
    map_range_off(CODE_SEGTAB, 0xC0000000u, 0xE0000000u, koff);
    map_range_off(DATA_SEGTAB, 0xC0000000u, 0xE0000000u, koff);
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
    if (seed_mapper) mem_w32(0xC0014038u, seed_mapper);
    printf("synthetic boot: %s map, tables at %08x (code) / %08x (data), "
           "%u page tables\n", realmm ? "direct(VA-0xC0000000)" : "identity",
           CODE_SEGTAB, DATA_SEGTAB, (ptpool_next - PTPOOL) / PAGE_SIZE);
}

/*
 * TCS (Test and Control System) stub.
 *
 * The TCS is reached through a shared-memory mailbox, not port I/O.  A pointer
 * to it sits at 0xC100CAA4 and points at 0xFE001800 in node-local device
 * space.  The handshake, read off _tcs_get_var:
 *
 *     byte 0 : command.  Kernel writes it, then spins until it reads back 0,
 *              meaning the TCS has consumed the request.
 *     byte 5 : response flags.  Kernel spins until bit 0 is set, then clears
 *              the byte.  Bit 1 means "error" (the caller returns 5).
 *
 * With no TCS present the kernel waits forever on the first spin.  This stub
 * completes the transaction immediately and reports success, which is enough
 * to let boot proceed; real variable payloads can be filled in as the kernel
 * turns out to need them.
 */
#define TCS_MBOX 0xFE001800u

static u64 tcs_commands;
static int tcs_trace;

static inline void tcs_poke(u32 a)
{
    if (a != TCS_MBOX) return;
    u8 cmd = mem_r8(TCS_MBOX);
    if (!cmd) return;
    if (tcs_trace)
        printf("[tcs] command %02x (args %08x %08x) -> ok\n", cmd,
               mem_r32(TCS_MBOX + 0x10), mem_r32(TCS_MBOX + 0x14));
    mem_w8(TCS_MBOX, 0);            /* consumed */
    mem_w8(TCS_MBOX + 5, 1);        /* response ready, no error */
    tcs_commands++;
}

static int cmmu_base_of(u32 a, u32 *base)
{
    for (int i = 0; i < n_cmmu; i++)
        if (a >= cmmu_present[i] && a < cmmu_present[i] + 0x1000) {
            *base = cmmu_present[i];
            return 1;
        }
    return 0;
}

/*
 * Interleaver stub.
 *
 * The kernel relocates its page tables into interleaved memory (physical mode
 * 2, 0x8xxxxxxx) and points the APR there -- but on a single-node machine
 * there is nothing to interleave across, and the emulator has no separate
 * backing for that space, so the walk reads zeros.  A scan of all 5481 live
 * pages confirms the kernel never actually populated a table there.
 *
 * With one node, interleaved memory is just local memory under a different
 * name, so an empty mode-2 segment table falls back to the identity table the
 * synthetic boot loader built.  This is a stand-in for a real interleaver
 * model, not a substitute for one.
 */
static int ileave_stub;
static u64 ileave_redirects;

/* Two-level MC88200 walk: area pointer -> segment table -> page table. */
static int mmu_walk(u32 apr, u32 vaddr, u32 *phys)
{
    u32 segtab = apr & 0xFFFFF000u;
    if (!segtab) return 0;
    if (ileave_stub && !realmm && (segtab & 0xC0000000u) == 0x80000000u &&
        !mem_r32(segtab + ((vaddr >> 22) & 0x3FF) * 4)) {
        segtab = DATA_SEGTAB;
        ileave_redirects++;
    }
    u32 sdesc = mem_r32(segtab + ((vaddr >> 22) & 0x3FF) * 4);
    if (!(sdesc & 1)) return 0;                       /* segment invalid */
    u32 pgtab = sdesc & 0xFFFFF000u;
    u32 pdesc = mem_r32(pgtab + ((vaddr >> 12) & 0x3FF) * 4);
    if (!(pdesc & 1)) return 0;                       /* page invalid */
    *phys = (pdesc & 0xFFFFF000u) | (vaddr & 0xFFF);
    return 1;
}

static void cmmu_command(u32 base, u32 cmd)
{
    /* 0x20/0x24 are the user/supervisor address-probe commands */
    if (cmd != 0x20 && cmd != 0x24) {
        if (mmu_trace) printf("[cmmu] %08x: unhandled command %02x\n", base, cmd);
        return;
    }
    u32 vaddr = mem_r32(base + CMMU_SAR);
    u32 apr   = mem_r32(base + (cmd == 0x24 ? CMMU_SAPR : CMMU_UAPR));
    u32 phys  = 0;
    if (mmu_walk(apr, vaddr, &phys)) {
        mem_w32(base + CMMU_SSR, 1);                  /* bit 0 = valid */
        mem_w32(base + CMMU_SAR, phys);
        probe_hits++;
        if (mmu_trace) printf("[cmmu] probe %08x -> %08x (apr %08x)\n", vaddr, phys, apr);
    } else {
        mem_w32(base + CMMU_SSR, 0);
        mem_w32(base + CMMU_PFAR, vaddr);
        probe_misses++;
        if (mmu_trace) printf("[cmmu] probe %08x MISS (apr %08x)\n", vaddr, apr);
    }
}

/*
 * Address translation.
 *
 * Off by default: the kernel boots to its banner without it, because early
 * boot runs on physical addresses.  Once the kernel installs its own page
 * tables in interleaved memory it expects translation to be live, so this is
 * needed to go further.  Enabled with --translate.
 *
 * Supervisor accesses use SAPR; bit 0 of the APR is the translation-enable
 * flag, and with it clear the address passes through untouched.  A tiny
 * direct-mapped TLB keeps the walk off the hot path.
 */
static int translate_on;
static u64 xlat_faults;
static u32 last_fault_va, last_fault_pc;

#define TLB_BITS 12
#define TLB_SIZE (1u << TLB_BITS)
static struct { u32 tag, pa; } tlb[2][TLB_SIZE];   /* [code/data][index] */

static void tlb_flush(void)
{
    memset(tlb, 0, sizeof tlb);
}

static inline u32 translate(u32 va, int code)
{
    if (!translate_on) return va;
    u32 apr = mem_r32((code ? 0xFFF7F000u : 0xFFF7E000u) + CMMU_SAPR);
    if (!(apr & 1)) return va;                 /* translation disabled */

    u32 vpn = va >> 12;
    u32 idx = vpn & (TLB_SIZE - 1);
    if (tlb[code][idx].tag == (vpn | 0x80000000u))
        return tlb[code][idx].pa | (va & 0xFFF);

    u32 pa;
    if (!mmu_walk(apr, va, &pa)) {
        /* device space (>= 0xE0000000) is fixed-physical, identity-mapped --
           don't require page tables for it */
        if (realmm && va >= 0xE0000000u) return va;
        xlat_faults++;
        last_fault_va = va;
        last_fault_pc = cpu.pc;
        /* realmm has no identity fallback -- a miss is a genuine unmapped
           access; log the first few so we can see where it happens */
        if (realmm && xlat_faults <= 20)
            printf("[xlat MISS] va=%08x pc=%08x apr=%08x @%llu\n",
                   va, cpu.pc, apr, (unsigned long long)cpu.count);
        return realmm ? cphys(va) : va;
    }
    tlb[code][idx].tag = vpn | 0x80000000u;
    tlb[code][idx].pa  = pa & 0xFFFFF000u;
    return pa;
}

static inline void dev_write32(u32 a, u32 v)
{
    if (sysmode) {
        {
            u32 cb;
            if (batc_trace && cmmu_base_of(a, &cb) && (a - cb) >= 0x400 && (a - cb) < 0x500)
                printf("[batc] %08x off=0x%x <- %08x\n", a, a - cb, v);
        }
        if (a == CMRAM_SELECT) { cmram_sel = v; return; }
        if (a == CMRAM_DATA) { mem_w32(cmram_sel & ~3u, v); cmram_writes++; return; }
    }
    mem_w32(a, v);
    if (sysmode) {
        tcs_poke(a);
        u32 base;
        if (cmmu_base_of(a, &base)) {
            u32 off = a - base;
            if (off == CMMU_SCR) cmmu_command(base, v);
            else if (mmu_trace && (off == CMMU_SAPR || off == CMMU_UAPR))
                printf("[cmmu] %08x %s <- %08x   (pc=%08x)\n", base,
                       off == CMMU_SAPR ? "SAPR" : "UAPR", v, cpu.pc);
        }
    }
}

/* Execute one instruction.  Returns 1 on trap. */
static int step(void)
{
    u32 pc = cpu.pc;
    dbg_pc = pc; dbg_count = cpu.count;

    if (sysmode && force_sig_pc && pc == force_sig_pc)
        cpu.r[2] = force_sig_val;

    if (watch_pc && pc == watch_pc) {
        printf("[watch] pc=%08x r2=%08x r3=%08x r7=%08x r9=%08x "
               "r24=%08x r25=%08x r27=%08x\n",
               pc, RD(2), RD(3), RD(7), RD(9), RD(24), RD(25), RD(27));
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
        goto badinsn;                     /* not implemented; none seen yet */
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
        } else if (sub >= 0x30 && sub <= 0x33) {              /* jmp / jsr    */
            delayed = sub & 1;
            if (sub >= 0x32) WR(1, delayed ? pc + 8 : pc + 4);
            branch = b & ~3u;
            taken = 1;
        } else if (sub == 0x3F) {                             /* rte          */
            trap_taken = 1; trap_vector = (u32)-2; trap_pc = pc; return 1;
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

/* --------------------------------------------------------------- a.out ---- */

#define HDR_PAGE 8192

typedef struct {
    u32 magic, text, data, bss, syms, entry, trsize, drsize;
    u8 *img;
    long size;
} AOut;

static u32 be32(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static int aout_load(const char *path, AOut *a)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return -1; }
    fseek(f, 0, SEEK_END); a->size = ftell(f); fseek(f, 0, SEEK_SET);
    a->img = malloc(a->size);
    if (fread(a->img, 1, a->size, f) != (size_t)a->size) { fclose(f); return -1; }
    fclose(f);
    a->magic = be32(a->img);      a->text  = be32(a->img + 4);
    a->data  = be32(a->img + 8);  a->bss   = be32(a->img + 12);
    a->syms  = be32(a->img + 16); a->entry = be32(a->img + 20);
    a->trsize = be32(a->img + 24); a->drsize = be32(a->img + 28);
    if ((a->magic & 0xFFFF) != 0x010B) {
        fprintf(stderr, "%s: not ZMAGIC (%08x)\n", path, a->magic);
        return -1;
    }
    return 0;
}

/* -------------------------------------------------------- user-mode kernel */

#define STACK_TOP 0x7FFF0000u

static FILE *fds[64];
static int   next_fd = 3;
static int   exited = -1;
static int   sys_err;          /* errno, or 0 for success */
static int   verbose_sys;

static void do_syscall(void)
{
    u32 n = RD(9);
    u32 a0 = RD(2), a1 = RD(3), a2 = RD(4), a3 = RD(5);
    u32 ret = 0;
    const char *nm = "?";
    sys_err = 0;

    switch (n) {
    case 0:                                   /* indir: real number in a0   */
        cpu.r[9] = a0;
        cpu.r[2] = RD(3); cpu.r[3] = RD(4); cpu.r[4] = RD(5); cpu.r[5] = RD(6);
        do_syscall();
        return;
    case 1: nm = "exit"; exited = (int)a0; return;
    case 3: {                                  /* read */
        nm = "read";
        FILE *f = (a0 < 64) ? fds[a0] : NULL;
        if (!f) { ret = 0; break; }
        static u8 buf[65536];
        size_t k = a2 > sizeof buf ? sizeof buf : a2;
        size_t g = fread(buf, 1, k, f);
        for (size_t i = 0; i < g; i++) mem_w8(a1 + (u32)i, buf[i]);
        ret = (u32)g; break;
    }
    case 4: {                                  /* write */
        nm = "write";
        for (u32 i = 0; i < a2; i++) fputc(mem_r8(a1 + i), a0 == 1 ? stdout : stderr);
        fflush(a0 == 1 ? stdout : stderr);
        ret = a2; break;
    }
    case 5: {                                  /* open */
        nm = "open";
        char path[512]; mem_cstr(a0, path, sizeof path);
        FILE *f = fopen(path, "rb");
        if (!f) { sys_err = 2; break; }
        fds[next_fd] = f; ret = next_fd++; break;
    }
    case 6: nm = "close";
        if (a0 < 64 && fds[a0]) { fclose(fds[a0]); fds[a0] = NULL; }
        break;
    case 19: { nm = "lseek";
        FILE *f = (a0 < 64) ? fds[a0] : NULL;
        if (f) { fseek(f, (long)a1, (int)a3); ret = (u32)ftell(f); }
        break; }
    case 17: nm = "obreak"; break;
    case 20: case 39: nm = "getpid"; ret = 4242; break;
    case 23: nm = "getuid"; break;
    case 45: nm = "getgid"; break;
    case 35: nm = "sync"; break;
    case 52: nm = "ioctl"; break;
    case 64: nm = "getpagesize"; ret = 4096; break;
    case 107: case 108: nm = "sigvec"; break;
    case 109: nm = "sigblock"; break;
    case 110: nm = "sigsetmask"; break;
    case 115: case 116: {                      /* gettimeofday */
        nm = "gettimeofday";
        time_t t = time(NULL);
        mem_w32(a0, (u32)t); mem_w32(a0 + 4, 0);
        if (a1) { mem_w32(a1, 0); mem_w32(a1 + 4, 0); }
        break; }
    case 121: {                                /* writev */
        nm = "writev";
        u32 total = 0;
        for (u32 i = 0; i < a2; i++) {
            u32 base = mem_r32(a1 + 8 * i), len = mem_r32(a1 + 8 * i + 4);
            for (u32 j = 0; j < len; j++)
                fputc(mem_r8(base + j), a0 == 1 ? stdout : stderr);
            total += len;
        }
        fflush(a0 == 1 ? stdout : stderr);
        ret = total; break; }
    case 38: case 39 + 100: case 62:
    case 187: case 188: case 189:              /* stat / lstat / fstat */
        nm = "stat*"; mem_zero(a1, 64); break;
    default:
        nm = "unimplemented";
        break;
    }
    if (verbose_sys)
        fprintf(stderr, "    [syscall %3u %-14s (%#x, %#x, %#x) = %u%s]\n",
                n, nm, a0, a1, a2, ret, sys_err ? " ERR" : "");
    WR(2, sys_err ? (u32)sys_err : ret);
}

static u32 build_stack(int argc, char **argv)
{
    u32 sp = STACK_TOP;
    u32 ptr[64];
    for (int i = 0; i < argc; i++) {
        size_t l = strlen(argv[i]) + 1;
        sp -= (u32)l;
        mem_load(sp, (const u8 *)argv[i], l);
        ptr[i] = sp;
    }
    sp &= ~7u;
    u32 nwords = 1 + argc + 1;
    sp -= 4 * nwords;
    sp &= ~7u;
    mem_w32(sp, (u32)argc);
    for (int i = 0; i < argc; i++) mem_w32(sp + 4 + 4 * i, ptr[i]);
    mem_w32(sp + 4 + 4 * argc, 0);
    return sp;
}

static int run_user(const char *path, int argc, char **argv, u64 limit)
{
    AOut a;
    if (aout_load(path, &a)) return 1;
    mem_load(0, a.img + HDR_PAGE, a.text);
    u32 dv = (a.text + 4095) & ~4095u;
    mem_load(dv, a.img + HDR_PAGE + a.text, a.data);
    mem_zero(dv + a.data, a.bss);

    printf("%s\n  text %u @ 00000000  data %u @ %08x  bss %u  entry %08x\n",
           path, a.text, a.data, dv, a.bss, a.entry);

    memset(&cpu, 0, sizeof cpu);
    cpu.pc = a.entry;
    WR(31, build_stack(argc, argv));

    printf("--- output ------------------------------------------------\n");
    while (cpu.count < limit) {
        if (step()) {
            if (trap_vector == 128) {
                do_syscall();
                if (exited >= 0) {
                    printf("\n-----------------------------------------------------------\n");
                    printf("exited(%d) after %llu instructions\n",
                           exited, (unsigned long long)cpu.count);
                    return 0;
                }
                /* success skips the error branch: resume at pc+8 */
                cpu.pc = trap_pc + (sys_err ? 4 : 8);
                trap_taken = 0;
            } else {
                printf("\n*** trap %d at pc=%08x after %llu instructions\n",
                       (int)trap_vector, trap_pc, (unsigned long long)cpu.count);
                return 1;
            }
        }
    }
    printf("\n(instruction limit reached at pc=%08x)\n", cpu.pc);
    return 0;
}

/* ------------------------------------------------------------ system mode */

#define TEXT_BASE 0xC0010000u
#define DATA_BASE 0xC1000000u

static int run_sys(const char *path, u64 limit, u32 sig)
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

    memset(&cpu, 0, sizeof cpu);
    cpu.pc = a.entry;
    WR(31, DATA_BASE + a.data + a.bss + 0x8000);

    /* Kernel message capture.  subr_prf.o's formatter is reached two ways:
       log(tag, fmt, ...) at LOG_ROUTINE, and printf_throttle(fmt, args)
       called directly by the panic path.  Hooking both shows everything the
       kernel tries to say, without needing a working console device. */
    clock_t t0 = clock();
    u32 last_fmt = 0;
    while (cpu.count < limit) {
        if (log_msgs) {
            if (cpu.pc == LOG_ROUTINE) {
                char fmt[200];
                mem_cstr(RD(3), fmt, sizeof fmt);
                if (RD(3) != last_fmt) {
                    /* format with the varargs the caller passed in r4.. */
                    int argi = 4;
                    printf("[kern] ");
                    for (char *p = fmt; *p; p++) {
                        if (*p != '%') { putchar(*p); continue; }
                        p++;
                        while (*p && strchr("-0123456789.l", *p)) p++;
                        u32 v = (argi <= 9) ? RD(argi++) : 0;
                        switch (*p) {
                        case 'd': printf("%d", (int)v); break;
                        case 'u': printf("%u", v); break;
                        case 'x': printf("%x", v); break;
                        case 'c': putchar((int)v); break;
                        case 's': { char b[160]; mem_cstr(v, b, sizeof b);
                                    fputs(b, stdout); } break;
                        case '%': putchar('%'); argi--; break;
                        default: putchar('%'); if (*p) putchar(*p); argi--;
                        }
                    }
                    if (!strchr(fmt, '\n')) putchar('\n');
                    last_fmt = RD(3);
                }
            } else if (cpu.pc == PRINTF_THROTTLE) {
                char fmt[160];
                mem_cstr(RD(2), fmt, sizeof fmt);
                if (RD(2) != last_fmt && fmt[0]) {
                    printf("[kern] %s", fmt);
                    if (!strchr(fmt, '\n')) putchar('\n');
                    /* resolve a %s argument: the caller hands over a small
                       descriptor, so scan it for a pointer into kernel data
                       that resolves to printable text */
                    if (strstr(fmt, "%s")) {
                        for (int k = 0; k < 6; k++) {
                            u32 p = mem_r32(RD(3) + 4 * k);
                            if (p < 0xC1000000u || p > 0xC1030000u) continue;
                            char s[120];
                            if (mem_cstr(p, s, sizeof s) < 3) continue;
                            int ok = 1;
                            for (char *q = s; *q; q++)
                                if (*q < 32 || *q > 126) { ok = 0; break; }
                            if (ok && strcmp(s, fmt)) {
                                printf("[kern]   -> \"%s\"\n", s);
                                break;
                            }
                        }
                    }
                    last_fmt = RD(2);
                }
            }
        }
        if (step()) {
            printf("trap %d at pc=%08x after %llu instructions\n",
                   (int)trap_vector, trap_pc, (unsigned long long)cpu.count);
            break;
        }
    }
    for (int k = 0; k < 32; k += 4) {
        printf("  ");
        for (int j = k; j < k + 4; j++) printf("r%-2d=%08x  ", j, RD(j));
        putchar('\n');
    }
    /*
     * Locate the kernel's real page tables.  It built them while translation
     * was effectively identity, so they are physically wherever it wrote
     * them -- not at the address its APR now advertises.  Scan every
     * allocated page as a candidate segment table: does its entry for
     * findpt_va point at a page table whose entry for findpt_va is valid?
     */
    if (findpt_va) {
        u32 sidx = (findpt_va >> 22) & 0x3FF, pidx = (findpt_va >> 12) & 0x3FF;
        unsigned live = 0, hits = 0;
        printf("searching for a segment table mapping %08x "
               "(seg %u, page %u)\n", findpt_va, sidx, pidx);
        for (u32 i = 0; i < NPAGES; i++) {
            if (!pages[i]) continue;
            live++;
            u32 cand = i << 12;
            u32 sdesc = mem_r32(cand + sidx * 4);
            if (!(sdesc & 1)) continue;
            u32 pgtab = sdesc & 0xFFFFF000u;
            if (!pages[pgtab >> 12]) continue;
            u32 pdesc = mem_r32(pgtab + pidx * 4);
            if (!(pdesc & 1)) continue;
            if (hits++ < 8)
                printf("  segtab %08x -> pgtab %08x -> pte %08x (pa %08x)\n",
                       cand, pgtab, pdesc,
                       (pdesc & 0xFFFFF000u) | (findpt_va & 0xFFF));
        }
        printf("  %u candidate segment tables among %u live pages\n",
               hits, live);
    }

    if (dump_addr) {
        printf("memory at %08x:\n", dump_addr);
        for (int k = 0; k < 4; k++) {
            printf("  %08x ", dump_addr + k * 16);
            for (int j = 0; j < 16; j += 4)
                printf(" %08x", mem_r32(dump_addr + k * 16 + j));
            putchar('\n');
        }
    }
    printf("tcs commands: %llu\n", (unsigned long long)tcs_commands);
    if (translate_on)
        printf("translation: %llu faults (last va %08x from pc %08x)\n",
               (unsigned long long)xlat_faults, last_fault_va, last_fault_pc);
    printf("cmmu probes: %llu hit, %llu miss\n",
           (unsigned long long)probe_hits, (unsigned long long)probe_misses);
    double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
    printf("stopped at pc=%08x after %llu instructions (%.2fs, %.1f Minsn/s)\n",
           cpu.pc, (unsigned long long)cpu.count, secs,
           secs > 0 ? cpu.count / secs / 1e6 : 0.0);
    return 0;
}

/* ------------------------------------------------------------------ main */

int main(int argc, char **argv)
{
    pages = calloc(NPAGES, sizeof *pages);
    if (!pages) { fprintf(stderr, "out of memory\n"); return 1; }

    u64 limit = 200000000ull;
    u32 sig = 0;
    int mode_sys = 0, i = 1;
    const char *path = NULL;

    /* Options may appear anywhere, including after the binary name, so parse
       the whole line first and collect non-option words separately. */
    char *words[64];
    int nwords = 0;
    for (; i < argc; i++) {
        if (!strncmp(argv[i], "--limit=", 8)) limit = strtoull(argv[i] + 8, 0, 0);
        else if (!strncmp(argv[i], "--sig=", 6)) sig = (u32)(u8)argv[i][6];
        else if (!strncmp(argv[i], "--scale=", 8)) tick_scale = (u32)strtoul(argv[i] + 8, 0, 0);
        else if (!strcmp(argv[i], "-v")) verbose_sys = 1;
        else if (!strcmp(argv[i], "--log")) log_msgs = 1;
        else if (!strcmp(argv[i], "--mmu")) mmu_trace = 1;
        else if (!strcmp(argv[i], "--batc")) batc_trace = 1;
        else if (!strcmp(argv[i], "--tcs")) tcs_trace = 1;
        else if (!strcmp(argv[i], "--translate")) translate_on = 1;
        else if (!strcmp(argv[i], "--realmm")) { realmm = 1; translate_on = 1; ileave_stub = 1; }
        else if (!strcmp(argv[i], "--dbgtrans")) dbg_trans = 1;
        else if (!strcmp(argv[i], "--ileave")) ileave_stub = 1;
        else if (!strncmp(argv[i], "--dump=", 7)) dump_addr = (u32)strtoul(argv[i]+7,0,0);
        else if (!strncmp(argv[i], "--findpt=", 9)) findpt_va = (u32)strtoul(argv[i]+9,0,0);
        else if (!strncmp(argv[i], "--watch=", 8)) watch_pc = (u32)strtoul(argv[i]+8,0,0);
        else if (!strncmp(argv[i], "--mapper=", 9)) seed_mapper = (u32)strtoul(argv[i]+9,0,0);
        else if (!strncmp(argv[i], "--wmem=", 7)) wmem_addr = (u32)strtoul(argv[i]+7,0,0);
        else if (!strncmp(argv[i], "--wrange=", 9)) {
            wmem_lo = (u32)strtoul(argv[i]+9,0,0);
            char *c = strchr(argv[i]+9, ':'); wmem_hi = c ? (u32)strtoul(c+1,0,0) : wmem_lo+0x1000;
        }
        else if (!strncmp(argv[i], "--flstride=", 11)) fl_stride = (u32)strtoul(argv[i]+11,0,0);
        else if (!strcmp(argv[i], "sys")) mode_sys = 1;
        else if (!strcmp(argv[i], "user")) mode_sys = 0;
        else if (nwords < 63) words[nwords++] = argv[i];
    }
    if (nwords) path = words[0];
    if (!path) {
        fprintf(stderr,
                "usage: nx88 user <binary> [args...] [-v] [--limit=N]\n"
                "       nx88 sys  <vmunix> [--sig=8] [--limit=N]\n");
        return 2;
    }
    if (mode_sys) return run_sys(path, limit, sig);

    return run_user(path, nwords, words, limit);
}
