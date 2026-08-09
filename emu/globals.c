/* All emulator machine state and configuration flags in one place.
   Declared extern in nx88.h; grouped here so the subsystem files
   hold only behaviour. */
#include "nx88.h"

u8 **pages;
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
int realmm;
int scsi_trace, scsi_trace_n;  /* --scsitrace: log VME controller I/O */
u32 irq_source;
u16 sha_status;
u64 dbg_count;                 /* current instruction count */
u32 dbg_pc;                    /* fwd: current pc, for trace prints */
CPU cpu;
/* trap reporting */
int   trap_taken;
u32   trap_vector;
u32   trap_pc;
/* forward decls so memop can service the free-running timer at all widths */
int  sysmode;
u32  tick_scale = 2000;
/* Completion status the SHA reports at base+0x73c after a queue-mode command.
   shareset does `bb0 5, status` and complains "start queue_mode: funny status
   0x%x" unless bit 5 is set -- which is why the old value of 1 produced exactly
   that message on every boot.  --shadone=N overrides for experiments. */
u32  sha_done_status = 0x20;
/* Bits a completion clears in the driver's descriptor at +4: bit 2 = error,
   and, with --scsi, bit 4 = command outstanding.  Clearing bit 4 gets shareset
   past "SHA_WORKQ_INIT still asserted" and into the target-probe loop -- which
   then forks a kernel process (procdup) whose child we cannot yet resume, so
   it stays opt-in until that works.  --shadesc=N overrides. */
u32  sha_desc_clear = 0x04;
const u8 memop_scale[16] = {
    1,4, 2,1, 8,4,2,1, 8,4,2,1, 8,4,2,1
};
/* system-mode device hooks (set by the sysmode driver; sysmode/tick_scale and
   TIMER_ADDR are forward-declared above so memop can see them) */
u32  force_sig_pc;      /* pc at which to force the TCS EEPROM sig */
u32  force_sig_val;
/*
 * Which CMMUs this node actually has.  Advertising one at every slot is wrong:
 * the boot path cross-checks the second code MMU against a per-node config
 * byte at 0xC00156B5, and panics "invalid code_mmu2" if hardware and config
 * disagree.  A minimal node is one code MMU plus one data MMU, which matches
 * that byte reading zero.
 */
u32 cmmu_present[8] = { 0xFFF7F000u, 0xFFF7E000u };
int n_cmmu = 2;
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
u32 cmram_sel;
u64 cmram_reads, cmram_writes;
u8 **cmram_pages;                  /* dedicated CMRAM backing store */
u32  watch_pc;
int  dump_pchist;
int  vmprobe;          /* --vmprobe: count kernel VM/fault entries (step-1 probe) */
int  vmexp;            /* --vmexp: at boot-complete, drive kernel VM funcs via RPC */
u32  procexp_cluster;  /* logical cluster our real process runs in */
u32  procexp_pid;      /* its pid: only it and its descendants get dispatched */  /* the logical cluster our real process runs in */
int  procexec;        /* --procexec: let the kernel's own execve load the binary */
int  procexp;         /* --procexp: create a real proc with the kernel's newproc */
int  ctxtrace;         /* --ctxtrace: log every distinct load_context() context */
u32  wmem_lo, wmem_hi; /* --wmem=A[:L]: log stores into this virtual range */
u64  wmem_max = 400;
u64  kwalk_user;       /* user-space walks resolved through the kernel's real tables */
int  dataphys;         /* --dataphys: load kernel data at the PA nX expects */
u32  kdata_va, kdata_off;  /* start VA and VA-PA offset of that region */
int  hwfault;          /* --hwfault: deliver real MC88100 access faults */
int  ufault_pending;   /* a real process faulted; run_sys pages it in and retries */
u32  ufault_va, ufault_pc;
int  ufault_code;
int  ufault_write;     /* the faulting access was a store (or xmem) */
u32  ufault_width;     /* its width in bytes (1/2/4/8) */
u64  ufaults;
u64  kdis_n, kfall_n;  /* kernel-VA table/linear disagreements; table misses */
u64  kdis_unused;           /* kernel VAs where table and linear map disagree */
u32  ktab_bias;        /* emulator-PA - kernel-PA for kernel page-table memory */
u32  stwatch_pc;       /* --stwatch=PC: log every store this function makes */
u32  regfind_val;      /* --regfind=V: log where a GPR first holds V */
int  quiet_uproc;      /* --quiet: no per-syscall / per-process chatter */
int  interactive;      /* --shell: hand the terminal to /bin/sh */
int  kmsgs;            /* --kmsg: echo the kernel's console output */
u32  brk_watch_pc, brk_watch_arg;
u32  last_sleep_chan, last_sleep_from;
u64  trace_len = 400;   /* --tracelen=N: PCs logged after a trap */
u32  cfg_nodes;                /* if >0, seed node-presence for N nodes */
int  deliver_traps;            /* deliver user traps to kernel handlers */
int  trace_traps;              /* log each delivered trap */
u64  trace_pc_until;           /* print PCs until this instruction count */
const char *uprog_path;      /* --uprog=PATH: real binary for proc1 */
u64  probe_hits, probe_misses;
int synth_boot = 1;
u32 ptpool_next = PTPOOL;
/*
 * Stride defaults to 8KB (page-granular, no 32-bit overflow across 96 slots).
 * The mapper geometry isn't fully pinned, but the value is not yet exercised:
 * every stride tried reaches the same next blocker, so the free list is used
 * only as a pop-able chain here, not yet as live DMA addresses.  Revisit when
 * the SCSI controller actually DMAs through a mapped window.
 */
u32 fl_stride = 0x2000;
u32 tcs_mbox_pa = TCS_MBOX;
u64 tcs_commands;
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
int ileave_stub;
u64 ileave_redirects;
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
int translate_on;
u64 xlat_faults;
u32 last_fault_va, last_fault_pc;
int clock_irq;
u64 clock_period = 100000, next_clock;
/* Pragmatic SCSI path.  The SHA driver rings the controller doorbell and then
   blocks in tsleep waiting for a completion interrupt.  Our boot runs in the
   non-sleepable idle-thread context (Mach MP bring-up never hands off to a
   bootstrap thread), so that tsleep panics.  Instead we complete SHA commands
   synchronously against disk.img and make the driver's wait return success.
   `sha_sync` enables it; addresses cover the sha.o driver text. */
int sha_sync = 1;
int biowait_sync = 1;
int skip_synchrtc = 1;         /* skip meaningless cross-node RTC sync */
/* Root filesystem backing.  During install the root is the tape's UFS image
   (tapeimage.img, superblock at byte 0x2000); the blank disk.img is the write
   target.  Reads issued by the buffer cache are satisfied from this file. */
const char *root_img_path = "../tapeimage.img";
FILE *root_img;
/* SCSI disk (sd0 = the install target) backing.  The SHA target-0 model routes
   READ/WRITE block I/O here; blank at first, newfs populates it. */
const char *disk_img_path = "disk.img";
FILE *disk_img;
/* The buffer's device pointer (struct buf +0x50) identifies which disk a
   buffer-cache read/write is for.  The first one seen during boot is the root
   device (the tape); any other is the SCSI disk (disk.img). */
u32  root_dev_vp;
FILE *fds[64];
int   next_fd = 3;
int   exited = -1;
int   sys_err;          /* errno, or 0 for success */
int   verbose_sys;
char kmsg_cons[KMSG_MAX], kmsg_log[KMSG_MAX], kmsg_last[KMSG_MAX];
int  kmsg_conslen, kmsg_loglen;
u32 upool_next = UPOOL_BASE;
int utest;
/* The segment table backing proc1's address space, once launch_utest builds it.
   Both UAPR and SAPR point here while proc1 runs (pmap_activate sets both), so
   a miss on either side with this APR is a fault in the user program. */
u32 usegtab_cur;
unsigned udemand_count;
int  console_io = 1;                 /* --no-console disables      */
int  console_port;                   /* --console-port=N: serve the interactive
                                        console on a loopback TCP socket (0=stdio) */
u8   fd_kernel[64];                  /* fd allocated by the kernel */
u8   fd_console[64] = { 1, 1, 1 };   /* which fds still reach the host */
u32  fd_watch_pc;                    /* trap pc of an fd-returning syscall */
int  fd_watch_pair;                  /* ...and it was pipe(2), returning two */
int  fd_watch_con;                   /* ...and the source was the console    */
u8   fd_watch_pipe;                  /* ...or a pipe                         */
u32  con_out_bytes, con_in_bytes;
Pipe pipes[MAX_PIPES];
u8   fd_pipe[64];                    /* fd -> pipe index + 1 */
u8   fd_disk[64];                    /* fd -> the raw sd0 device (routed to disk.img) */
u32  disk_off[64];                   /* per-fd byte offset into disk.img (lseek) */
int  fd_watch_disk;                  /* the fd-returning open was for the raw disk */
/* Host-file passthrough (--hostfile=PATH): the guest opens the synthetic path
   /hosttar and the emulator serves reads straight from this host file, so guest
   tools (tar) can consume an archive that lives on no filesystem.  Read-only. */
const char *hostfile_path;
const char *disk_mount;              /* --diskmount=/usr: where the guest mounts
                                        disk.img, so execve can map a guest path
                                        onto disk.img's FFS and load from there */
FILE *hostfile_img;
u32  hostfile_size;                  /* real byte length of the host file */
u32  hostfile_vsize;                 /* length presented to the guest: real size
                                        padded with zero blocks to a full tar
                                        record, so a terminator-less archive still
                                        ends in the two zero blocks tar expects */
u8   fd_host[64];                    /* fd -> the /hosttar passthrough */
u32  host_off[64];                   /* per-fd byte offset into hostfile_img */
const char *uargv[MAX_UARGV];
const char *uenvp[MAX_UENVP];
unsigned nuargv, nuenvp;
UProc uprocs[MAX_UPROC];
int   ucur = -1, next_pid = 2, uproc_on;
const char *guest_root = "..";        /* extracted tape = the guest's / */
int uproc_all_done;      /* set when the last process has exited */

/* Named replacements for the former anonymous-struct globals (see nx88.h). */
DevMap devmap[512];
int    ndevmap;
TlbEnt tlb[2][TLB_SIZE];
