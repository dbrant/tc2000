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
#include <math.h>

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
static u64 pcsample;
static int scsi_trace, scsi_trace_n;  /* --scsitrace: log VME controller I/O */

/* Node interrupt controller (0xE0780000): the handler reads the pending source
   from +0x18 and, via mask tables, routes bit 27 -> _hardclock.  Setting the
   source to 0x40 selects exactly that bit. */
#define IRQ_SOURCE_REG 0xE0780018u
static u32 irq_source;

/* SCSI Host Adapter (sha.o) probe stub.  The controller's dual-port RAM +
   register window sits at VME 0xFC008000; status reg at +0x800, command reg at
   +0x802.  _init_iopb resets it by pulsing the command reg 0x1000->0 and then
   polls status until it reads 2 (ready).  With no real controller the poll
   spins forever.  This stub models just the reset handshake: the pulse drives
   status busy(1)->ready(2), enough to get past _init_iopb and observe the next
   step (IOPB build + doorbell).  Full IOPB/DMA/interrupt modelling is later. */
#define SHA_BASE   0xFC008800u
#define SHA_STATUS 0xFC008800u
#define SHA_CMD    0xFC008802u
static u16 sha_status;

/* Per-node private-variable window.  _vvlocptr(var,node) maps a kernel-data
   template var in [0xC0014000,0xC00156DC) to 0xF9000000 + node*0x2000 +
   (var-0xC0014000): each node's private copy of that variable region.  The
   kernel initialises the *template* directly (e.g. _intrlv_size clears the
   50-entry exception table at 0xC0014118 to -1) and then reads it back through
   this window, so on the master node the window aliases its own .data
   template.  On a single node all nodes collapse onto that one copy. */
#define VV_WINDOW  0xF9000000u
#define VV_STRIDE  0x2000u
#define VV_TEMPLATE 0xC0014000u          /* template base VA (in .text image) */

static inline u32 cphys(u32 a)
{
    if (realmm && a >= 0x80000000u && a < 0xC0000000u)
        return a & 0x7FFFFFFFu;
    if (a >= VV_WINDOW && a < VV_WINDOW + 64u * VV_STRIDE) {
        u32 off = (a - VV_WINDOW) % VV_STRIDE;
        u32 tpl = realmm ? (VV_TEMPLATE - 0xC0000000u) : VV_TEMPLATE;
        return tpl + off;
    }
    return a;
}

static inline u8 *page_of(u32 a)
{
    u32 i = cphys(a) >> 12;
    u8 *p = pages[i];
    if (!p) { p = calloc(1, PAGE_SIZE); pages[i] = p; }
    return p;
}

static u32 wmem_addr;                 /* --wmem=ADDR: trace writes to a word */
static u32 wval;                      /* --wval=V: trace writes whose value&~0xfff==V */
static u32 wmem_lo, wmem_hi;          /* --wrange=LO:HI: trace writes in range */
static u64 dbg_count;                 /* current instruction count */

static u32 dbg_pc;                    /* fwd: current pc, for trace prints */
static u32 be32(const u8 *p);         /* fwd: big-endian word load */
static inline u8 mem_r8(u32 a) { return page_of(a)[a & 4095]; }
static inline void mem_w8(u32 a, u8 v)
{
    if (wmem_addr && (a >= (wmem_addr & ~3u) && a < (wmem_addr & ~3u) + 4))
        printf("[wmem8] %08x <- %02x  pc=%08x @%llu\n", a, v, dbg_pc,
               (unsigned long long)dbg_count);
    page_of(a)[a & 4095] = v;
}

static u32 mem_r32(u32 a)
{
    if ((a & 4095) <= 4092) {
        u8 *p = page_of(a) + (a & 4095);
        return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
    }
    return ((u32)mem_r8(a) << 24) | ((u32)mem_r8(a+1) << 16) |
           ((u32)mem_r8(a+2) << 8) | mem_r8(a+3);
}

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
static void mem_w16(u32 a, u16 v)
{
    if (wmem_addr && (a == wmem_addr || a + 1 == wmem_addr || a == wmem_addr + 2))
        printf("[wmem16] %08x <- %04x  pc=%08x @%llu\n", a, v, dbg_pc,
               (unsigned long long)dbg_count);
    mem_w8(a, v >> 8); mem_w8(a+1, v);
}

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

/* forward decls so memop can service the free-running timer at all widths */
static int  sysmode;
static u32  tick_scale = 2000;
/* Completion status the SHA reports at base+0x73c after a queue-mode command.
   shareset does `bb0 5, status` and complains "start queue_mode: funny status
   0x%x" unless bit 5 is set -- which is why the old value of 1 produced exactly
   that message on every boot.  --shadone=N overrides for experiments. */
static u32  sha_done_status = 0x20;
#define TIMER_ADDR 0xE07E8018u

/* memory-op helper shared by the immediate and triadic forms */
static inline u32 translate(u32 va, int code);

static void memop(u32 sub, u32 D, u32 ea)
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

static const u8 memop_scale[16] = {
    1,4, 2,1, 8,4,2,1, 8,4,2,1, 8,4,2,1
};

/* system-mode device hooks (set by the sysmode driver; sysmode/tick_scale and
   TIMER_ADDR are forward-declared above so memop can see them) */
static u32  force_sig_pc;      /* pc at which to force the TCS EEPROM sig */
static u32  force_sig_val;
static int  log_msgs;
#define LOG_ROUTINE     0xC005A85Cu
#define PRINTF_THROTTLE 0xC005A9D0u
#define KERN_PUTCHAR    0xC005B218u   /* subr_prf.o putchar(c, ...) */
#define SDSTRATEGY      0xC00BAFE4u   /* sd.o strategy(bp)          */
#define BIODONE         0xC0074138u   /* bio.o biodone(bp)          */
#define GETBLK_WAIT     0xC007350Cu   /* return addr of getblk's sleep-on-busy */

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
 * The select value keys a CMRAM slot.  The interleaver's config/pool RAM is
 * *separate* storage from main memory -- it must not alias it.  An earlier
 * model proxied the data port straight onto the main backing store, which was
 * fine for the sparse high selects the free list uses (0x40000000) but fatal
 * once _cmram_interleave_setup walked the low slots (0, 0x8000, 0x10000, ...):
 * writing 0 to slot 0x90000 landed on kernel text at physical 0x90000 and
 * silently zeroed vm_queue_remove, so it ran through no-op text, never loaded
 * r25, and eventually called vm_mapping_init(garbage) -> "vnode_mapping_decr".
 * CMRAM therefore gets its own sparse, on-demand backing keyed by the select.
 */
static u32 cmram_sel;
static u64 cmram_reads, cmram_writes;

static u8 **cmram_pages;                  /* dedicated CMRAM backing store */
static inline u8 *cmram_page_of(u32 sel)
{
    u32 i = (sel >> 12) & (NPAGES - 1);
    if (!cmram_pages[i]) cmram_pages[i] = calloc(1, PAGE_SIZE);
    return cmram_pages[i];
}
static inline u32 cmram_r32(u32 sel)
{
    u8 *p = cmram_page_of(sel) + (sel & (PAGE_SIZE - 4));
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}
static inline void cmram_w32(u32 sel, u32 v)
{
    u8 *p = cmram_page_of(sel) + (sel & (PAGE_SIZE - 4));
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

static inline u32 dev_read32(u32 a)
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
static int  dump_pchist;
static int  dump_uarea;
static int  quiet_uproc;      /* --quiet: no per-syscall / per-process chatter */
static int  interactive;      /* --shell: hand the terminal to /bin/sh */
static int  kmsgs;            /* --kmsg: echo the kernel's console output */
static int  brk_passthru;     /* --brk-passthru: let obreak reach the kernel */
static u32  brk_watch_pc, brk_watch_arg;
static u32  last_sleep_chan, last_sleep_from;
static u64  trace_len = 400;   /* --tracelen=N: PCs logged after a trap */
static u32  cfg_nodes;                /* if >0, seed node-presence for N nodes */
static int  uland_probe;              /* dump run queue at the swapper sleep */
static int  deliver_traps;            /* deliver user traps to kernel handlers */
static int  trace_traps;              /* log each delivered trap */
static u64  trace_pc_until;           /* print PCs until this instruction count */
static u32  utrap_vec = 128;        /* --utest syscall trap vector */
static const char *uprog_path;      /* --uprog=PATH: real binary for proc1 */
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
        /* the free list lives in the interleaver pool, reached through the
           CMRAM data port -- populate the dedicated CMRAM store, not main RAM */
        cmram_w32(slot, (i + 1 < n) ? head + (i + 1) * fl_stride : 0);
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
/* TCS mailbox physical address.  Identity boot accesses it at the fixed device
   address 0xFE001800; realmm maps it into a kernel VA that resolves elsewhere,
   so the address is tracked dynamically (set from the pointer the kernel
   stores at _pmap_kernel+0xd74).  Whatever it is, the kernel's writer and
   poller both use the same VA, so completing the handshake at the resolved PA
   makes both sides agree. */
#define TCS_MBOX 0xFE001800u
static u32 tcs_mbox_pa = TCS_MBOX;

static u64 tcs_commands;
static int tcs_trace;

static inline void tcs_poke(u32 a)
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

/*
 * Device-mapping intercept (realmm option b).
 *
 * The kernel maps device pages (physical >= 0xE0000000: DUART, interleaver,
 * TCS, CMMU windows) into kernel VA space via the low-level pmap mapper at
 * 0xC00A6B44 (r3 = VA, r4 = PA).  Those PTEs don't reliably land in the
 * kernel's own page table in our model, and the synthetic direct map would
 * resolve the VA to the wrong (offset) physical address -- which is why the
 * TCS mailbox VA pointed at plain RAM instead of the device.  So we snoop that
 * mapper and remember VA->devicePA here; walk_fb consults it before the
 * synthetic fallback.  Device PAs (>=0xE0000000) are unmistakable: the offset
 * map only ever yields < 0x20000000 for kernel space.
 */
#define PMAP_MAP_FN 0xC00A6B44u
static struct { u32 va, pa; } devmap[512];
static int ndevmap;

static void devmap_add(u32 va, u32 pa)
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

static u32 devmap_lookup(u32 va)
{
    va &= ~0xFFFu;
    for (int i = 0; i < ndevmap; i++)
        if (devmap[i].va == va) return devmap[i].pa;
    return 0;
}

/* Walk the kernel's active table, then (realmm) fall back to the synthetic
   direct-map table so the kernel's own vtop and the CPU's translate() agree. */
static int walk_fb(u32 apr, u32 vaddr, int code, u32 *phys)
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

static void cmmu_command(u32 base, u32 cmd)
{
    /* 0x20/0x24 are the user/supervisor address-probe commands */
    if (cmd != 0x20 && cmd != 0x24) {
        if (mmu_trace) printf("[cmmu] %08x: unhandled command %02x\n", base, cmd);
        return;
    }
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

static u32 xva;   /* --xva: log translations of this VA */
/* Demand-page the synthetic user address space (defined with the upool code
   below).  Returns a physical page for `va`, or 0 if `apr` is not ours. */
static u32 udemand_page(u32 apr, u32 va);
static inline u32 translate(u32 va, int code)
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
    if (xva && (va & ~0xFFFu) == (xva & ~0xFFFu)) {
        u32 tp; int ok = walk_fb(apr, va, code, &tp);
        printf("[xva] %08x -> %s pa=%08x apr=%08x pc=%08x @%llu\n",
               va, ok ? "ok" : "MISS", ok ? tp : 0, apr, cpu.pc,
               (unsigned long long)dbg_count);
    }
    if (!walk_fb(apr, va, code, &pa)) {
        /* Demand paging for our synthetic user space.  The kernel's VM has no
           idea this address space exists, so a real vector-2 fault would find
           no vm_map entry; instead we do what the pager would: hand out a
           zeroed page and map it.  This is what lets obreak's heap growth and
           deep stacks actually be touchable. */
        u32 dp = udemand_page(apr, va);
        if (dp) return dp | (va & 0xFFF);
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

static inline void dev_write32(u32 a, u32 v)
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
static void deliver_exception(u32 vector)
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
static void deliver_trap(u32 vector, u32 tpc)
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

static int clock_irq;
static u64 clock_period = 100000, next_clock;

/* Pragmatic SCSI path.  The SHA driver rings the controller doorbell and then
   blocks in tsleep waiting for a completion interrupt.  Our boot runs in the
   non-sleepable idle-thread context (Mach MP bring-up never hands off to a
   bootstrap thread), so that tsleep panics.  Instead we complete SHA commands
   synchronously against disk.img and make the driver's wait return success.
   `sha_sync` enables it; addresses cover the sha.o driver text. */
static int sha_sync = 1;
static int biowait_sync = 1;
static int skip_synchrtc = 1;         /* skip meaningless cross-node RTC sync */
/* Root filesystem backing.  During install the root is the tape's UFS image
   (tapeimage.img, superblock at byte 0x2000); the blank disk.img is the write
   target.  Reads issued by the buffer cache are satisfied from this file. */
static const char *root_img_path = "../tapeimage.img";
static FILE *root_img;
#define SHA_O_LO 0xC00BC000u
#define SHA_O_HI 0xC00BE100u
#define TSLEEP_ENTRY 0xC0054A64u
/* Process a pending SHA command.  The driver blocked in tsleep waiting for the
   completion interrupt handler (_shaintr) to fill in its command descriptor;
   since we short-circuit the wait, we stand in for _shaintr and mark the
   command complete.  The descriptor is in r27 for the SHA callers; [desc+4]
   bit 2 is the error flag -- clear it so the driver takes its success path. */
static void sha_complete(void)
{
    u32 desc = RD(27);
    if (desc >= 0xC0000000u) {
        u32 pa = translate(desc + 4, 0);
        mem_w32(pa, mem_r32(pa) & ~4u);      /* clear error bit (bit 2) */
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

/* MC88100 floating point (SFU1, opcode 0x21).  Precision codes: 0=single
   (one register), 1=double (register pair rn:rn+1, high word in rn), 2=extended
   (unused by nX -- treated as double).  Values are carried internally as C
   double.  This code is reached first by the scheduler's load-average update
   (_add_curproc), which is statistical and does not gate control flow, so exact
   rounding is not critical -- correctness of not trapping is what matters. */
static double fp_read(u32 reg, int prec)
{
    if (prec == 0) {
        u32 b = RD(reg); float f; memcpy(&f, &b, 4); return (double)f;
    }
    u64 b = ((u64)RD(reg) << 32) | RD(reg + 1);
    double d; memcpy(&d, &b, 8); return d;
}
static void fp_write(u32 reg, int prec, double v)
{
    if (prec == 0) {
        float f = (float)v; u32 b; memcpy(&b, &f, 4); WR(reg, b); return;
    }
    u64 b; memcpy(&b, &v, 8);
    WR(reg, (u32)(b >> 32)); WR(reg + 1, (u32)b);
}

/* Execute one instruction.  Returns 1 on trap. */
static int step(void)
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
    /* A miss is routine once a shell is running -- it is just PATH being
       searched -- so let the caller report it in the guest's own terms. */
    if (!f) { if (!quiet_uproc) perror(path); return -1; }
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

/* --------------------------------------------------- kernel console output
   subr_prf.o funnels every character a kernel printf produces through
   putchar(c, flags), so hooking it prints the kernel's own log fully
   formatted rather than reconstructing it from format strings.

   Each message goes through TWICE, though: once with flags=1 for the console
   and again with flags=4 for msgbuf/syslog.  Messages from log() with a
   priority (the `<6>...' ones) take only the second path.  So both streams are
   assembled into lines separately, and a syslog line is printed only if it
   isn't the one the console just showed -- which yields every message exactly
   once, console and syslog alike. */
#define KMSG_MAX 512
static char kmsg_cons[KMSG_MAX], kmsg_log[KMSG_MAX], kmsg_last[KMSG_MAX];
static int  kmsg_conslen, kmsg_loglen;

static void kmsg_line(const char *s, int is_log)
{
    if (is_log && !strcmp(s, kmsg_last)) return;      /* console already had it */
    printf("[nx] %s\n", s);
    fflush(stdout);
    if (!is_log) { strncpy(kmsg_last, s, KMSG_MAX - 1); kmsg_last[KMSG_MAX-1] = 0; }
}

static void kmsg_putchar(int c, u32 flags)
{
    int is_log = !(flags & 1);
    char *buf = is_log ? kmsg_log : kmsg_cons;
    int  *len = is_log ? &kmsg_loglen : &kmsg_conslen;
    if (c == '\n' || *len >= KMSG_MAX - 1) {
        buf[*len] = 0;
        if (*len) kmsg_line(buf, is_log);
        *len = 0;
        if (c != '\n' && c) buf[(*len)++] = (char)c;
        return;
    }
    if (c == '\r' || !c) return;
    buf[(*len)++] = (char)c;
}

static void kmsg_flush(void)
{
    if (kmsg_conslen) { kmsg_cons[kmsg_conslen] = 0; kmsg_line(kmsg_cons, 0); kmsg_conslen = 0; }
    if (kmsg_loglen)  { kmsg_log[kmsg_loglen]  = 0; kmsg_line(kmsg_log, 1);  kmsg_loglen  = 0; }
}

/* ------------------------------------------------------------ system mode */

/* ---- minimal userland bring-up (increment 2): drop a tiny program into user
   mode under the booted kernel, so its syscall traps exercise the trap-delivery
   path.  A private physical pool backs a synthetic user address space; UAPR
   points at it, translate() uses UAPR whenever PSR bit31 is clear. */
#define UPOOL_BASE 0xDD000000u
static u32 upool_next = UPOOL_BASE;
static int utest;
static u32 upool_alloc(void) { u32 p = upool_next; upool_next += PAGE_SIZE;
                               mem_zero(p, PAGE_SIZE); return p; }
static void umap(u32 segtab, u32 va, u32 pa)
{
    u32 sidx = (va >> 22) & 0x3FF;
    u32 sdesc = mem_r32(segtab + sidx * 4), pgtab;
    if (sdesc & 1) pgtab = sdesc & 0xFFFFF000u;
    else { pgtab = upool_alloc(); mem_w32(segtab + sidx * 4, pgtab | 1); }
    mem_w32(pgtab + ((va >> 12) & 0x3FF) * 4, (pa & 0xFFFFF000u) | 1);
}
/* The segment table backing proc1's address space, once launch_utest builds it.
   Both UAPR and SAPR point here while proc1 runs (pmap_activate sets both), so
   a miss on either side with this APR is a fault in the user program. */
static u32 usegtab_cur;
static unsigned udemand_count;
static u32 udemand_page(u32 apr, u32 va)
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

/* ---------------------------------------------------------------- console
   nX's console is driven through the TCS mailbox, which we model only far
   enough to boot -- so there is no character device a process could open for
   its terminal.  Rather than build one, we service the three standard
   descriptors in the emulator, exactly as the buffer cache's disk reads are
   serviced: intercept read/write/ioctl/close on fds 0-2 at the syscall trap and
   satisfy them against the host's own stdin/stdout.

   The one hazard is fd numbering.  proc1 starts with an empty descriptor table,
   so the kernel's first real open() hands back fd 0 -- which would then collide
   with our stdin.  So we track ownership: any descriptor the kernel allocates
   is marked kernel-owned and passes straight through, and close() releases it
   again.  Only descriptors the kernel has NOT allocated are treated as the
   console. */
static int  console_io = 1;                 /* --no-console disables      */
static u8   fd_kernel[64];                  /* fd allocated by the kernel */
static u8   fd_console[64] = { 1, 1, 1 };   /* which fds still reach the host */
/* The bootstrap stub (see launch_utest) lives here and ends with this private
   syscall number, which the process manager turns into the first exec. */
#define UAREA_VA   0xFBFFE000u          /* _u: the per-process kernel state */
#define UAREA_SIZE 0x2000u
#define UBOOT_VA   0x7F000000u
#define UBRK_TOP   0x00400000u          /* data segment granted to the kernel */
#define UBOOT_DONE 0x3F0u
static u32  fd_watch_pc;                    /* trap pc of an fd-returning syscall */
static int  fd_watch_pair;                  /* ...and it was pipe(2), returning two */
static int  fd_watch_con;                   /* ...and the source was the console    */
static u8   fd_watch_pipe;                  /* ...or a pipe                         */
static u32  con_out_bytes, con_in_bytes;

/* ------------------------------------------------------------------- pipes
   A pipe is the one thing a shell script blocks on, and blocking is exactly
   what we cannot do: the kernel would try to sleep on behalf of what it still
   believes is the idle process, and panic.  So pipes are serviced here as
   well.  The kernel's own pipe(2) still runs -- that is what reserves the two
   descriptor numbers and makes dup2 and close behave -- but the bytes flow
   through a buffer of ours.

   The buffer is unbounded, so a writer never blocks, and a read of an empty
   pipe is end-of-file rather than a wait.  That is the right answer here
   precisely because processes run one at a time and a child runs before its
   parent: by the time anyone reads, everyone who could have written has. */
#define MAX_PIPES 32
typedef struct { int used; u8 *buf; size_t len, cap, rpos; } Pipe;
static Pipe pipes[MAX_PIPES];
static u8   fd_pipe[64];                    /* fd -> pipe index + 1 */

static int pipe_alloc(void)
{
    for (int i = 0; i < MAX_PIPES; i++)
        if (!pipes[i].used) {
            pipes[i].used = 1; pipes[i].len = pipes[i].rpos = 0;
            if (!pipes[i].buf) { pipes[i].cap = 65536; pipes[i].buf = malloc(pipes[i].cap); }
            return i;
        }
    return -1;
}

static void pipe_write(Pipe *p, const u8 *src, u32 n)
{
    if (p->len + n > p->cap) {
        while (p->len + n > p->cap) p->cap *= 2;
        p->buf = realloc(p->buf, p->cap);
    }
    memcpy(p->buf + p->len, src, n);
    p->len += n;
}

static int fd_is_console(u32 fd)
{
    return console_io && fd < 64 && fd_console[fd];
}

/* Service a syscall entirely in the emulator.  Returns 1 if handled, and
   leaves the result in r2 with the pc advanced past the error branch. */
static int console_syscall(u32 sysno, u32 tpc)
{
    u32 a0 = RD(2), a1 = RD(3), a2 = RD(4);
    long ret;
    /* Pipe traffic first: these descriptors are real kernel descriptors, but
       the bytes are ours (see the pipe notes above). */
    if ((sysno == 3 || sysno == 4) && a0 < 64 && fd_pipe[a0]) {
        Pipe *p = &pipes[fd_pipe[a0] - 1];
        if (sysno == 4) {
            u8 *tmp = malloc(a2 ? a2 : 1);
            for (u32 i = 0; i < a2; i++) tmp[i] = mem_r8(translate(a1 + i, 0));
            pipe_write(p, tmp, a2);
            free(tmp);
            ret = (long)a2;
        } else {
            u32 avail = (u32)(p->len - p->rpos), n = a2 < avail ? a2 : avail;
            for (u32 i = 0; i < n; i++)
                mem_w8(translate(a1 + i, 0), p->buf[p->rpos + i]);
            p->rpos += n;
            ret = (long)n;                             /* 0 == end of file */
        }
        WR(2, (u32)ret);
        cpu.pc = tpc + 8;
        return 1;
    }
    switch (sysno) {
    case 4:                                            /* write(fd, buf, n) */
        if (!fd_is_console(a0)) return 0;
        for (u32 i = 0; i < a2 && i < (1u << 20); i++)
            fputc(mem_r8(translate(a1 + i, 0)), a0 == 2 ? stderr : stdout);
        fflush(a0 == 2 ? stderr : stdout);
        con_out_bytes += a2;
        ret = (long)a2;
        break;
    case 3: {                                          /* read(fd, buf, n)  */
        if (!fd_is_console(a0)) return 0;
        u32 n = a2 > 4096 ? 4096 : a2;
        u8 tmp[4096];
        size_t got = n ? fread(tmp, 1, 1, stdin) : 0;   /* line-ish: 1 byte at a time */
        while (got && got < n && tmp[got - 1] != '\n') {
            size_t g2 = fread(tmp + got, 1, 1, stdin);
            if (!g2) break;
            got += g2;
        }
        for (size_t i = 0; i < got; i++) mem_w8(translate(a1 + (u32)i, 0), tmp[i]);
        con_in_bytes += (u32)got;
        ret = (long)got;
        break;
    }
    case 54:                                         /* ioctl(fd, ...)    */
        if (!fd_is_console(a0)) return 0;
        ret = 0;                                       /* yes, it's a tty   */
        break;
    default:
        return 0;
    }
    WR(2, (u32)ret);
    cpu.pc = tpc + 8;                                  /* success return    */
    return 1;
}

/* Write into the synthetic user address space (walking our own segment table). */
static void uwrite8(u32 segtab, u32 va, u8 b)
{
    u32 pa;
    if (mmu_walk(segtab | 1u, va, &pa)) mem_w8(pa, b);
}
static void uwrite32(u32 segtab, u32 va, u32 v)
{
    uwrite8(segtab, va,     (u8)(v >> 24)); uwrite8(segtab, va + 1, (u8)(v >> 16));
    uwrite8(segtab, va + 2, (u8)(v >> 8));  uwrite8(segtab, va + 3, (u8)v);
}

/* Load a real nX a.out into the user address space, the same layout run_user
   uses: text at VA 0, data page-aligned after it, then bss, and a stack topped
   at STACK_TOP with argc/argv.  Backing pages come from our private pool and
   are mapped through the synthetic user segment table. */
#define MAX_UARGV 32
#define MAX_UENVP 32
static const char *uargv[MAX_UARGV];
static const char *uenvp[MAX_UENVP];
static unsigned nuargv, nuenvp;

static int load_user_prog_av(const char *path, u32 segtab, u32 *entry, u32 *sp_out,
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

/* ------------------------------------------------------- process management
   nX's own fork/exec/exit operate on Mach proc + thread structures that our
   synthetic proc1 only approximates; exit() alone walks the parent pointer, the
   run queues and the pmap, and spins forever on a lock the moment one of them
   is not what it expects.  Building all of that faithfully is a project in
   itself, and it is not what makes the install tape interesting.

   So process *lifetime* is modelled in the emulator -- fork, execve, wait and
   exit -- while everything that touches the machine or the filesystem
   (open/read/lseek/stat/ioctl on the real UFS root) still goes to the real
   kernel.  This is the same division of labour the buffer cache and the console
   already use, and it is what lets /bin/sh run a script.

   Processes run one at a time and switch only at a syscall boundary in user
   mode, so no kernel state is ever in flight across a switch: the whole context
   is the 32 GPRs, the PC, and the address space. */
#define MAX_UPROC 32
typedef struct {
    int used, zombie, waiting, status;
    int pid, ppid;
    u32 wait_statusp;                 /* wait4's status pointer, if blocked */
    u32 brk;                          /* heap break -- ours, not the kernel's */
    u32 segtab, pc, r[32];
    /* The u-area is the per-process kernel state -- descriptor table, cwd,
       signal disposition, limits.  Our processes are not real nX processes, so
       nothing swaps it for them; we save and restore it around a switch, which
       is precisely what makes an fd opened by one process private to it. */
    u8  uarea[UAREA_SIZE];
    u8  fdcon[64];                    /* its view of fd_console */
    u8  fdpipe[64];                   /* ...and of fd_pipe        */
} UProc;
static UProc uprocs[MAX_UPROC];
static int   ucur = -1, next_pid = 2, uproc_on;
static const char *guest_root = "..";        /* extracted tape = the guest's / */

/* Map a guest path onto the extracted tape directory that mirrors the root
   filesystem the kernel is reading blocks from. */
static const char *guest_path(const char *p)
{
    static char buf[1024];
    snprintf(buf, sizeof buf, "%s%s%s", guest_root,
             (*p == '/') ? "" : "/", p);
    return buf;
}

static int uread_str(u32 va, char *buf, size_t n)
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
static u32 aspace_clone(u32 src)
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

static void uproc_activate(u32 segtab)
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

/* u_ofile, the descriptor table, found by opening three files from the
   bootstrap stub and looking for consecutive kernel pointers in the u-area
   (--uarea dumps it).  Each entry is a struct file: f_flag at +0x08, then
   f_type and f_count as shorts at +0x0c.  fork has to bump f_count for every
   open descriptor, or the first child to close one frees a file the other
   processes still point at. */
#define U_OFILE   0x748u
#define U_NOFILE  64
#define F_COUNT   0x0eu

static void uarea_dup_files(void)
{
    for (int i = 0; i < U_NOFILE; i++) {
        u32 f = mem_r32(UAREA_VA + U_OFILE + 4 * i);
        if (f < 0xC0000000u || f >= 0xC8000000u) continue;
        u32 pa = translate(f + F_COUNT, 0);
        mem_w16(pa, (u16)(mem_r16(pa) + 1));
    }
}

static void uarea_save(UProc *u)
{
    for (u32 i = 0; i < UAREA_SIZE; i++) u->uarea[i] = mem_r8(UAREA_VA + i);
    memcpy(u->fdcon, fd_console, sizeof fd_console);
    memcpy(u->fdpipe, fd_pipe, sizeof fd_pipe);
}
static void uarea_load(const UProc *u)
{
    for (u32 i = 0; i < UAREA_SIZE; i++) mem_w8(UAREA_VA + i, u->uarea[i]);
    memcpy(fd_console, u->fdcon, sizeof fd_console);
    memcpy(fd_pipe, u->fdpipe, sizeof fd_pipe);
}

static void uctx_save(u32 pc)
{
    if (ucur < 0) return;
    UProc *u = &uprocs[ucur];
    u->pc = pc;
    for (int i = 0; i < 32; i++) u->r[i] = RD(i);
    uarea_save(u);
}

static void uctx_load(int i)
{
    UProc *u = &uprocs[i];
    ucur = i;
    for (int k = 1; k < 32; k++) WR(k, u->r[k]);
    cpu.pc = u->pc;
    cpu.cr[1] = 0;                                  /* user mode */
    uarea_load(u);
    uproc_activate(u->segtab);
}

static int uproc_new(int ppid, u32 segtab)
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

static int uproc_runnable(void)
{
    for (int i = 0; i < MAX_UPROC; i++)
        if (uprocs[i].used && !uprocs[i].zombie && !uprocs[i].waiting) return i;
    return -1;
}

static int uproc_find_pid(int pid)
{
    for (int i = 0; i < MAX_UPROC; i++)
        if (uprocs[i].used && uprocs[i].pid == pid) return i;
    return -1;
}

static int uproc_all_done;      /* set when the last process has exited */

/* Service the process-lifetime syscalls.  Returns 1 if handled; the pc is left
   at tpc+8 (success) or tpc+4 (error) exactly as the kernel's return path
   would leave it. */
static int uproc_syscall(u32 sysno, u32 tpc)
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
static u32 create_proc1(u32 user_pc, u32 user_sp, u32 usegtab)
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

static void launch_utest(void)
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

    /* Kernel message capture.  subr_prf.o's formatter is reached two ways:
       log(tag, fmt, ...) at LOG_ROUTINE, and printf_throttle(fmt, args)
       called directly by the panic path.  Hooking both shows everything the
       kernel tries to say, without needing a working console device. */
    clock_t t0 = clock();
    u32 last_fmt = 0;
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
            break;
        }
        /* BOOT COMPLETE: the swapper's sched() idle loop is `for(;;)
           sleep(&proc0)` -- _sleep entry c0054720 called from c004859c with
           chan &proc0 (=c0047f88).  Reaching it means main() finished: root is
           mounted, kernel/daemon threads are created, and proc0 goes idle.
           (proc0 can't actually sleep -- it's still on the run queue -- so
           without this catcher it would crash into doadump; stop cleanly here
           and report the milestone instead.) */
        if (cpu.pc == 0xC0054720u && RD(1) == 0xC004859Cu && RD(2) == 0xC0047F88u
            && !uland_probe) {
            if (kmsgs) kmsg_flush();
            printf("[boot-complete] kernel reached the swapper sched() idle "
                   "loop @%llu -- main() done, root mounted.\n",
                   (unsigned long long)cpu.count);
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
        if (pcsample && (cpu.count % pcsample) == 0)
            printf("[pc] @%llu pc=%08x\n",
                   (unsigned long long)cpu.count, cpu.pc);
        /* Kernel console output.  subr_prf.o funnels every character printf
           produces through _putchar(c, ...), whatever the message started out
           as -- so hooking it prints the kernel's own log, fully formatted,
           instead of reconstructing it from format strings.  The real putchar
           goes on to msgbuf and cnputc; we only watch. */
        if (kmsgs && cpu.pc == KERN_PUTCHAR)
            kmsg_putchar((int)(RD(2) & 0xff), RD(3));
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
            }
            if (fd_watch_pair && RD(3) < 64 && RD(2) < 64) {
                fd_kernel[RD(3)] = 1;                  /* pipe(2): two fds */
                int pi = pipe_alloc();                 /* ...one buffer     */
                if (pi >= 0) fd_pipe[RD(2)] = fd_pipe[RD(3)] = (u8)(pi + 1);
            }
            fd_watch_pc = 0; fd_watch_pair = 0; fd_watch_con = 0;
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
                    /* dup: whatever the new descriptor turns out to be, it is
                       the host's terminal exactly when the source was.  The
                       shell reads its input through a dup of fd 0, so without
                       this its `read` builtin sees EOF. */
                    fd_watch_con = (RD(9) == 41 && RD(2) < 64) ? fd_console[RD(2)] : 0;
                    fd_watch_pipe = (RD(9) == 41 && RD(2) < 64) ? fd_pipe[RD(2)] : 0;
                }
                if (trap_vector == 128 && !(cpu.cr[1] & 0x80000000u) && RD(9) == 6
                    && RD(2) < 64)
                    fd_kernel[RD(2)] = fd_console[RD(2)] = fd_pipe[RD(2)] = 0;
                /* dup2 replaces the target outright, terminal-ness included --
                   this is how a shell redirect takes stdout off the console. */
                if (trap_vector == 128 && !(cpu.cr[1] & 0x80000000u)
                    && RD(9) == 90 && RD(3) < 64 && RD(2) < 64)
                    fd_console[RD(3)] = fd_console[RD(2)],
                    fd_pipe[RD(3)] = fd_pipe[RD(2)];
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
    if (!quiet_uproc)
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

/* ------------------------------------------------------------------ main */

int main(int argc, char **argv)
{
    pages = calloc(NPAGES, sizeof *pages);
    cmram_pages = calloc(NPAGES, sizeof *cmram_pages);
    if (!pages) { fprintf(stderr, "out of memory\n"); return 1; }

    u64 limit = 200000000ull;
    u32 sig = 'A';                     /* node EEPROM signature: 16MB + vmebus present */
    int mode_sys = 0, identity_mode = 0, i = 1;
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
        else if (!strcmp(argv[i], "--kmsg")) kmsgs = 1;
        else if (!strncmp(argv[i], "--shadone=", 10)) sha_done_status = (u32)strtoul(argv[i]+10,0,0);
        else if (!strcmp(argv[i], "--brk-passthru")) brk_passthru = 1;
        else if (!strcmp(argv[i], "--mmu")) mmu_trace = 1;
        else if (!strcmp(argv[i], "--batc")) batc_trace = 1;
        else if (!strcmp(argv[i], "--tcs")) tcs_trace = 1;
        else if (!strcmp(argv[i], "--translate")) translate_on = 1;
        else if (!strcmp(argv[i], "--realmm")) { realmm = 1; translate_on = 1; ileave_stub = 1; }
        else if (!strcmp(argv[i], "--identity")) identity_mode = 1;
        else if (!strcmp(argv[i], "--dbgtrans")) dbg_trans = 1;
        else if (!strcmp(argv[i], "--scsitrace")) scsi_trace = 1;
        else if (!strcmp(argv[i], "--clock")) clock_irq = 1;
        else if (!strncmp(argv[i], "--clock=", 8)) { clock_irq = 1; clock_period = strtoull(argv[i]+8,0,0); }
        else if (!strncmp(argv[i], "--pcsample=", 11)) pcsample = strtoull(argv[i]+11,0,0);
        else if (!strcmp(argv[i], "--ileave")) ileave_stub = 1;
        else if (!strncmp(argv[i], "--dump=", 7)) dump_addr = (u32)strtoul(argv[i]+7,0,0);
        else if (!strncmp(argv[i], "--findpt=", 9)) findpt_va = (u32)strtoul(argv[i]+9,0,0);
        else if (!strncmp(argv[i], "--watch=", 8)) watch_pc = (u32)strtoul(argv[i]+8,0,0);
        else if (!strcmp(argv[i], "--pchist")) dump_pchist = 1;
        else if (!strcmp(argv[i], "--uarea")) dump_uarea = 1;
        else if (!strncmp(argv[i], "--tracelen=", 11)) trace_len = strtoull(argv[i]+11, 0, 0);
        else if (!strcmp(argv[i], "--uland")) uland_probe = 1;
        else if (!strcmp(argv[i], "--deliver-traps")) deliver_traps = 1;
        else if (!strcmp(argv[i], "--trace-traps")) { deliver_traps = 1; trace_traps = 1; }
        else if (!strcmp(argv[i], "--utest")) { utest = 1; deliver_traps = 1; trace_traps = 1; }
        else if (!strcmp(argv[i], "--no-console")) console_io = 0;
        else if (!strcmp(argv[i], "--quiet")) trace_traps = 0, quiet_uproc = 1;
        /* --shell: boot, then hand the terminal to /bin/sh off the tape.  Just
           the convenience settings -- the program, a login-ish environment,
           quiet output, and a budget big enough to sit at a prompt. */
        else if (!strcmp(argv[i], "--shell")) {
            interactive = 1; utest = 1; deliver_traps = 1;
            trace_traps = 0; quiet_uproc = 1;
            limit = ~0ull;
        }
        else if (!strncmp(argv[i], "--uarg=", 7) && nuargv < MAX_UARGV)
            uargv[nuargv++] = argv[i] + 7;
        else if (!strncmp(argv[i], "--uenv=", 7) && nuenvp < MAX_UENVP)
            uenvp[nuenvp++] = argv[i] + 7;
        else if (!strncmp(argv[i], "--utrap=", 8)) utrap_vec = (u32)strtoul(argv[i]+8,0,0);
        else if (!strncmp(argv[i], "--uprog=", 8)) { uprog_path = argv[i]+8; utest = 1; deliver_traps = 1; trace_traps = 1; }
        else if (!strncmp(argv[i], "--mapper=", 9)) seed_mapper = (u32)strtoul(argv[i]+9,0,0);
        else if (!strncmp(argv[i], "--wmem=", 7)) wmem_addr = (u32)strtoul(argv[i]+7,0,0);
        else if (!strncmp(argv[i], "--xva=", 6)) xva = (u32)strtoul(argv[i]+6,0,0);
        else if (!strncmp(argv[i], "--wval=", 7)) wval = (u32)strtoul(argv[i]+7,0,0) & 0xFFFFF000u;
        else if (!strncmp(argv[i], "--wrange=", 9)) {
            wmem_lo = (u32)strtoul(argv[i]+9,0,0);
            char *c = strchr(argv[i]+9, ':'); wmem_hi = c ? (u32)strtoul(c+1,0,0) : wmem_lo+0x1000;
        }
        else if (!strncmp(argv[i], "--flstride=", 11)) fl_stride = (u32)strtoul(argv[i]+11,0,0);
        else if (!strncmp(argv[i], "--root=", 7)) root_img_path = argv[i]+7;
        else if (!strncmp(argv[i], "--nodes=", 8)) cfg_nodes = (u32)strtoul(argv[i]+8,0,0);
        else if (!strcmp(argv[i], "sys")) mode_sys = 1;
        else if (!strcmp(argv[i], "user")) mode_sys = 0;
        else if (nwords < 63) words[nwords++] = argv[i];
    }
    if (nwords) path = words[0];
    if (!path) {
        fprintf(stderr,
                "usage: nx88 user <binary> [args...] [-v] [--limit=N]\n"
                "       nx88 sys  <vmunix> [--limit=N] [--identity]\n"
                "  sys mode defaults to the real-memory model (--realmm) with\n"
                "  EEPROM signature 'A'; pass --identity for the old identity path.\n");
        return 2;
    }
    /* realmm is the primary, sound path -- default it on in system mode.
       --identity selects the superseded identity+fallback path instead. */
    if (mode_sys) {
        translate_on = 1;
        ileave_stub  = 1;
        realmm = !identity_mode;
        /* Open the root filesystem image (the tape's UFS) for disk reads.
           If --root wasn't given, derive it from the vmunix path: vmunix lives
           at <tapedir>/vmunix and the tape image is its sibling <tapedir>.img
           (e.g. .../tapeimage/vmunix -> .../tapeimage.img). */
        static char derived[1024], gr[1024];
        {   /* the extracted tape directory doubles as the guest's / for exec */
            size_t n = strlen(path);
            const char *b = path + n;
            while (b > path && b[-1] != '/' && b[-1] != '\\') b--;
            size_t dlen = (size_t)(b - path);
            if (dlen > 1 && dlen < sizeof gr) {
                memcpy(gr, path, dlen - 1); gr[dlen - 1] = 0;
                guest_root = gr;
            }
        }
        /* --shell: everything else follows from the tape directory. */
        if (interactive && !uprog_path) {
            static char shpath[1024];
            snprintf(shpath, sizeof shpath, "%s/bin/sh", guest_root);
            uprog_path = shpath;
            if (!nuargv) uargv[nuargv++] = "sh";
            if (!nuenvp) {
                uenvp[nuenvp++] = "PATH=/bin:/etc:/usr/bin";
                uenvp[nuenvp++] = "HOME=/";
                uenvp[nuenvp++] = "TERM=dumb";
                uenvp[nuenvp++] = "SHELL=/bin/sh";
            }
        }
        if (!strcmp(root_img_path, "../tapeimage.img")) {
            size_t n = strlen(path);
            const char *base = path + n;
            while (base > path && base[-1] != '/' && base[-1] != '\\') base--;
            size_t dlen = (size_t)(base - path);   /* includes trailing slash */
            if (dlen > 1 && dlen < sizeof derived - 8) {
                memcpy(derived, path, dlen - 1);   /* drop the trailing slash */
                strcpy(derived + dlen - 1, ".img");
                root_img_path = derived;
            }
        }
        root_img = fopen(root_img_path, "rb");
        if (root_img)
            printf("root image: %s (open)\n", root_img_path);
        else
            fprintf(stderr, "root image: %s could not be opened (disk reads "
                    "will return zeros) -- pass --root=PATH\n", root_img_path);
    }
    if (mode_sys) return run_sys(path, limit, sig);

    return run_user(path, nwords, words, limit);
}
