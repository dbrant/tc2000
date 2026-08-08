/*
 * nx88.h -- shared declarations for the nX / MC88100 emulator.
 *
 * The emulator was one 3000-line file; it is now split by theme:
 *
 *   globals.c   all machine state and configuration flags (see externs below)
 *   memory.c    paged physical RAM, CMRAM backing store, byte helpers
 *   devices.c   memory-mapped device models: SHA/SCSI, timer, TCS, CMMU IDs,
 *               CMRAM window, node IRQ -- and memop, the load/store dispatch
 *   mmu.c       88200 translation, page-table walk, TLB, CMMU probe command,
 *               the synthetic boot loader (segment/page tables, free list)
 *   cpu.c       instruction decode/execute (step), cmp/bitfield/ff/FPU,
 *               exception + rte + trap delivery
 *   aout.c      a.out loader
 *   kmsg.c      capture of the kernel's own console output (--kmsg)
 *   usermode.c  standalone user-mode runner (a binary with no kernel)
 *   console.c   under-kernel host I/O: stdin/stdout/stderr and pipes
 *   proc.c      under-kernel process lifetime: demand paging, the process
 *               table (fork/exec/wait/exit), proc1 synthesis, program loading
 *   sysmode.c   run_sys -- the system-mode driver loop
 *   main.c      entry point, argument parsing, mode dispatch
 *
 * The hot memory accessors stay inline here so every file inlines them; the
 * larger device/translate functions are ordinary cross-file calls.
 */
#ifndef NX88_H
#define NX88_H

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

/* ------------------------------------------------------------- constants */
#define PAGE_SIZE 4096
#define NPAGES    (1u << 20)          /* 4 GiB of address space, sparse */
#define IRQ_SOURCE_REG 0xE0780018u
#define SHA_BASE   0xFC008800u
#define SHA_STATUS 0xFC008800u
#define SHA_CMD    0xFC008802u
#define VV_WINDOW  0xF9000000u
#define VV_STRIDE  0x2000u
#define VV_TEMPLATE 0xC0014000u
#define TIMER_ADDR 0xE07E8018u
#define KERN_PUTCHAR    0xC005B218u   /* subr_prf.o putchar(c, ...) */
#define SDSTRATEGY      0xC00BAFE4u   /* sd.o strategy(bp)          */
#define BIODONE         0xC0074138u   /* bio.o biodone(bp)          */
#define GETBLK_WAIT     0xC007350Cu   /* return addr of getblk's sleep-on-busy */
#define CMMU_BASE 0xFFF00000u
#define CMRAM_SELECT 0xE07EA00Cu
#define CMRAM_DATA   0xE07EB000u
#define CMMU_SCR  0x004
#define CMMU_SSR  0x008
#define CMMU_SAR  0x00C
#define CMMU_PFAR 0x108
#define CMMU_SAPR 0x200
#define CMMU_UAPR 0x204
#define CODE_SEGTAB 0xE0790000u
#define DATA_SEGTAB 0x803F0000u
#define PTPOOL      0xDF000000u          /* above the kernel's memory */
#define KOFF 0xC0000000u                 /* kernel VA -> PA offset */
#define TCS_MBOX 0xFE001800u
#define PMAP_MAP_FN 0xC00A6B44u
#define TLB_BITS 12
#define TLB_SIZE (1u << TLB_BITS)
#define SHA_O_LO 0xC00BC000u
#define SHA_O_HI 0xC00BE100u
#define TSLEEP_ENTRY 0xC0054A64u
#define HDR_PAGE 8192
#define STACK_TOP 0x7FFF0000u
#define KMSG_MAX 512
#define UPOOL_BASE 0xDD000000u
#define UAREA_VA   0xFBFFE000u          /* _u: the per-process kernel state */
#define UAREA_SIZE 0x2000u
#define UBOOT_VA   0x7F000000u
#define UBRK_TOP   0x00400000u          /* data segment granted to the kernel */
#define UBOOT_DONE 0x3F0u
#define MAX_PIPES 32
#define MAX_UARGV 32
#define MAX_UENVP 32
#define MAX_UPROC 32
#define U_OFILE   0x748u
#define U_NOFILE  64
#define F_COUNT   0x0eu
#define TEXT_BASE 0xC0010000u
#define DATA_BASE 0xC1000000u

/* --------------------------------------------------------------- types */
typedef struct {
    u32 r[32];
    u32 cr[64];
    u32 pc;
    u32 pending;          /* delay-slot branch target */
    int has_pending;
    u64 count;
} CPU;

typedef struct {
    u32 magic, text, data, bss, syms, entry, trsize, drsize;
    u8 *img;
    long size;
} AOut;

typedef struct { int used; u8 *buf; size_t len, cap, rpos; } Pipe;

typedef struct {
    int used, zombie, waiting, status;
    int pid, ppid;
    u32 wait_statusp;
    u32 brk;
    u32 segtab, pc, r[32];
    u8  uarea[UAREA_SIZE];
    u8  fdcon[64];
    u8  fdpipe[64];
} UProc;

typedef struct { u32 va, pa; } DevMap;    /* device VA->PA map entry */
typedef struct { u32 tag, pa; } TlbEnt;   /* per-side TLB entry      */

/* cmp condition bit positions */
enum { C_EQ = 2, C_NE = 3, C_GT = 4, C_LE = 5, C_LT = 6,
       C_GE = 7, C_HI = 8, C_LS = 9, C_LO = 10, C_HS = 11 };

#define RD(n)      ((n) ? cpu.r[n] : 0u)
#define WR(n, v)   do { if (n) cpu.r[n] = (u32)(v); } while (0)

/* ------------------------------------------------ shared machine state */
/* ---- globals ---- */
extern u8 **pages;
extern int realmm;
extern int scsi_trace, scsi_trace_n;
extern u32 irq_source;
extern u16 sha_status;
extern u64 dbg_count;
extern u32 dbg_pc;
extern CPU cpu;
extern int trap_taken;
extern u32 trap_vector;
extern u32 trap_pc;
extern int sysmode;
extern u32 tick_scale ;
extern u32 sha_done_status ;
extern u32 sha_desc_clear ;
extern const u8 memop_scale[] ;
extern u32 force_sig_pc;
extern u32 force_sig_val;
extern u32 cmmu_present[] ;
extern int n_cmmu ;
extern u32 cmram_sel;
extern u64 cmram_reads, cmram_writes;
extern u8 **cmram_pages;
extern u32 watch_pc;
extern int dump_pchist;
extern int quiet_uproc;
extern int interactive;
extern int kmsgs;
extern u32 brk_watch_pc, brk_watch_arg;
extern u32 last_sleep_chan, last_sleep_from;
extern u64 trace_len ;
extern u32 cfg_nodes;
extern int deliver_traps;
extern int trace_traps;
extern u64 trace_pc_until;
extern const char *uprog_path;
extern u64 probe_hits, probe_misses;
extern int synth_boot ;
extern u32 ptpool_next ;
extern u32 fl_stride ;
extern u32 tcs_mbox_pa ;
extern u64 tcs_commands;
extern int ileave_stub;
extern u64 ileave_redirects;
extern int translate_on;
extern u64 xlat_faults;
extern u32 last_fault_va, last_fault_pc;
extern int clock_irq;
extern u64 clock_period , next_clock;
extern int sha_sync ;
extern int biowait_sync ;
extern int skip_synchrtc ;
extern const char *root_img_path ;
extern FILE *root_img;
extern const char *disk_img_path ;
extern FILE *disk_img;
extern u32 root_dev_vp;
extern FILE *fds[];
extern int next_fd ;
extern int exited ;
extern int sys_err;
extern int verbose_sys;
extern char kmsg_cons[], kmsg_log[], kmsg_last[];
extern int kmsg_conslen, kmsg_loglen;
extern u32 upool_next ;
extern int utest;
extern u32 usegtab_cur;
extern unsigned udemand_count;
extern int console_io ;
extern int console_port ;
extern u8 fd_kernel[];
extern u8 fd_console[64];
extern u32 fd_watch_pc;
extern int fd_watch_pair;
extern int fd_watch_con;
extern u8 fd_watch_pipe;
extern u32 con_out_bytes, con_in_bytes;
extern Pipe pipes[];
extern u8 fd_pipe[64];
extern u8 fd_disk[64];
extern u32 disk_off[64];
extern const char *hostfile_path;
extern FILE *hostfile_img;
extern u32 hostfile_size, hostfile_vsize;
extern const char *disk_mount;      /* --diskmount: guest mount point of disk.img */
extern u8 fd_host[64];
extern u32 host_off[64];
extern int fd_watch_disk;
extern const char *uargv[];
extern const char *uenvp[];
extern unsigned nuargv, nuenvp;
extern UProc uprocs[MAX_UPROC];
extern int ucur , next_pid , uproc_on;
extern const char *guest_root ;
extern int uproc_all_done;


/* -------------------------------------------- named-struct machine state */
extern DevMap devmap[512];
extern int    ndevmap;
extern TlbEnt tlb[2][TLB_SIZE];

/* ----------------------------------------- hot inline memory accessors ---
   Kept inline (and thus in the header) because they are on the per-instruction
   fast path; everything heavier is an ordinary cross-file call. */
static inline u32 be32(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

/* Fold interleaved/vv-window physical addresses onto the single node's RAM. */
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

static inline u8 mem_r8(u32 a) { return page_of(a)[a & 4095]; }

static inline void mem_w8(u32 a, u8 v)
{
    page_of(a)[a & 4095] = v;
}

static inline u32 mem_r32(u32 a)
{
    if ((a & 4095) <= 4092) {
        u8 *p = page_of(a) + (a & 4095);
        return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
    }
    return ((u32)mem_r8(a) << 24) | ((u32)mem_r8(a+1) << 16) |
           ((u32)mem_r8(a+2) << 8) | mem_r8(a+3);
}

static inline void mem_w32(u32 a, u32 v)
{
    if ((a & 4095) <= 4092) {
        u8 *p = page_of(a) + (a & 4095);
        p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
        return;
    }
    mem_w8(a, v >> 24); mem_w8(a+1, v >> 16);
    mem_w8(a+2, v >> 8); mem_w8(a+3, v);
}

static inline u16 mem_r16(u32 a) { return ((u16)mem_r8(a) << 8) | mem_r8(a+1); }

static inline void mem_w16(u32 a, u16 v)
{
    mem_w8(a, v >> 8); mem_w8(a+1, v);
}

/* ------------------------------------------------------ cross-module API */
void mem_load(u32 a, const u8 *src, size_t n);
void mem_zero(u32 a, size_t n);
int mem_cstr(u32 a, char *out, int max);
u32 do_cmp(u32 a, u32 b);
int cond_true(u32 m5, u32 a);
void bitfield(u32 sub, u32 D, u32 a, u32 width, u32 offset);
void memop(u32 sub, u32 D, u32 ea);
int is_cmmu_id(u32 a);
u32 cmmu_id_for(u32 a);
u8 *cmram_page_of(u32 sel);
u32 cmram_r32(u32 sel);
void cmram_w32(u32 sel, u32 v);
u32 dev_read32(u32 a);
void map_range_off(u32 segtab, u32 lo, u32 hi, u32 off);
void map_range(u32 segtab, u32 lo, u32 hi);
void build_free_list(void);
void boot_build_tables(void);
void tcs_poke(u32 a);
int cmmu_base_of(u32 a, u32 *base);
int mmu_walk(u32 apr, u32 vaddr, u32 *phys);
void devmap_add(u32 va, u32 pa);
u32 devmap_lookup(u32 va);
int walk_fb(u32 apr, u32 vaddr, int code, u32 *phys);
void cmmu_command(u32 base, u32 cmd);
void tlb_flush(void);
u32 translate(u32 va, int code);
void dev_write32(u32 a, u32 v);
void deliver_exception(u32 vector);
void deliver_trap(u32 vector, u32 tpc);
void sha_complete(void);
void sha_sdcomplete(u32 cmd);
void sd_ensure_label(void);
double fp_read(u32 reg, int prec);
void fp_write(u32 reg, int prec, double v);
int step(void);
int aout_load(const char *path, AOut *a);
int aout_load_mem(u8 *img, u32 size, AOut *a);
int ffs_read_file(const char *path, u8 **buf, u32 *len);
void do_syscall(void);
u32 build_stack(int argc, char **argv);
int run_user(const char *path, int argc, char **argv, u64 limit);
void kmsg_line(const char *s, int is_log);
void kmsg_putchar(int c, u32 flags);
void kmsg_flush(void);
u32 upool_alloc(void);
void umap(u32 segtab, u32 va, u32 pa);
u32 udemand_page(u32 apr, u32 va);
int pipe_alloc(void);
void pipe_write(Pipe *p, const u8 *src, u32 n);
int fd_is_console(u32 fd);
int console_syscall(u32 sysno, u32 tpc);
void console_listen(int port);
void con_write_str(const char *s);
int disk_syscall(u32 sysno, u32 tpc);
int hostfile_syscall(u32 sysno, u32 tpc);
void uwrite8(u32 segtab, u32 va, u8 b);
void uwrite32(u32 segtab, u32 va, u32 v);
int load_user_prog_av(const char *path, u32 segtab, u32 *entry, u32 *sp_out, const char **av, unsigned nav, const char **ev, unsigned nev);
const char *guest_path(const char *p);
int uread_str(u32 va, char *buf, size_t n);
u32 aspace_clone(u32 src);
void uproc_activate(u32 segtab);
void uarea_dup_files(void);
void uarea_save(UProc *u);
void uarea_load(const UProc *u);
void uctx_save(u32 pc);
void uctx_load(int i);
int uproc_new(int ppid, u32 segtab);
int uproc_runnable(void);
int uproc_find_pid(int pid);
int uproc_syscall(u32 sysno, u32 tpc);
u32 create_proc1(u32 user_pc, u32 user_sp, u32 usegtab);
void launch_utest(void);
int run_sys(const char *path, u64 limit, u32 sig);

#endif /* NX88_H */
