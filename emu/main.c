/* Generated split of the former single-file nx88.c.
   Shared types, macros, hot inline accessors, and all
   cross-module declarations live in nx88.h. */
#include "nx88.h"

int main(int argc, char **argv)
{
    /* Line-buffer stdout.  Every diagnostic here (--trace-traps, --wmem,
       --pwatch, panic reports) goes to stdout, and when the run is redirected
       to a file stdio switches to BLOCK buffering -- so a run that ends by
       being killed, or that stops mid-investigation, loses the last and most
       interesting few kilobytes.  Two separate debugging sessions have been
       sent down the wrong path by exactly that. */
    setvbuf(stdout, 0, _IOLBF, 0);

    pages = calloc(NPAGES, sizeof *pages);
    cmram_pages = calloc(NPAGES, sizeof *cmram_pages);
    if (!pages) { fprintf(stderr, "out of memory\n"); return 1; }

    u64 limit = 200000000ull;
    u32 sig = 'A';                     /* node EEPROM signature: 16MB + vmebus present */
    int mode_sys = 0, identity_mode = 0, i = 1, tickdiv_set = 0;
    const char *path = NULL;

    /* Options may appear anywhere, including after the binary name, so parse
       the whole line first and collect non-option words separately. */
    char *words[64];
    int nwords = 0;
    for (; i < argc; i++) {
        /* --- run configuration --- */
        if      (!strncmp(argv[i], "--limit=", 8)) limit = strtoull(argv[i] + 8, 0, 0);
        else if (!strncmp(argv[i], "--sig=", 6))   sig = (u32)(u8)argv[i][6];
        else if (!strncmp(argv[i], "--nodes=", 8)) cfg_nodes = (u32)strtoul(argv[i]+8,0,0);
        else if (!strncmp(argv[i], "--tape=", 7))  root_img_path = argv[i]+7;
        else if (!strncmp(argv[i], "--disk=", 7))  disk_img_path = argv[i]+7;
        else if (!strncmp(argv[i], "--hostfile=", 11)) hostfile_path = argv[i]+11;
        else if (!strcmp(argv[i], "--scsi"))       sha_desc_clear = 0x14;
        else if (!strcmp(argv[i], "--identity"))   identity_mode = 1;
        else if (!strcmp(argv[i], "--clock"))      clock_irq = 1;
        else if (!strncmp(argv[i], "--clock=", 8)) { clock_irq = 1; clock_period = strtoull(argv[i]+8,0,0); }
        else if (!strncmp(argv[i], "--tickdiv=", 10))
            { tick_div = (u32)strtoul(argv[i]+10,0,0); tickdiv_set = 1; }
        /* --- user-mode program launch --- */
        /* --shell: boot, then hand the terminal to the guest's own /bin/sh,
           run as a REAL nX process.  It used to mean the synthetic process
           model; that model is gone, so this is now just a set of defaults
           over the real process path. */
        else if (!strcmp(argv[i], "--shell")) {
            interactive = 1; deliver_traps = 1;
            trace_traps = 0; quiet_uproc = 1;
            limit = ~0ull;
        }
        else if (!strncmp(argv[i], "--uprog=", 8))     { uprog_path = argv[i]+8; deliver_traps = 1; trace_traps = 1; debug = 1; }
        else if (!strncmp(argv[i], "--uarg=", 7) && nuargv < MAX_UARGV) uargv[nuargv++] = argv[i] + 7;
        else if (!strncmp(argv[i], "--uenv=", 7) && nuenvp < MAX_UENVP) uenvp[nuenvp++] = argv[i] + 7;
        /* --- output / diagnostics ---
           ★ EVERY flag in this section that produces output also sets `debug`.
           The emulator's own commentary is off by default now (see dbg() in
           nx88.h), and a diagnostic flag whose output the same run then
           swallowed would be a trap.  So asking for any of it asks for all of
           it, which is also what these flags did before the gate existed --
           no command line documented in BOOT.md prints less than it used to.
           --quiet and --kmsg are the two that deliberately do NOT: --quiet
           means be quiet, and --kmsg asks for the KERNEL's voice, which is
           exactly the thing that is no longer buried. */
        else if (!strcmp(argv[i], "--quiet"))      { trace_traps = 0; quiet_uproc = 1; debug = 0; }
        else if (!strcmp(argv[i], "--debug"))      debug = 1;
        else if (!strcmp(argv[i], "--kmsg"))       kmsgs = 1;
        else if (!strcmp(argv[i], "--scsitrace"))  { scsi_trace = 1; debug = 1; }
        else if (!strcmp(argv[i], "--pchist"))     { dump_pchist = 1; debug = 1; }
        else if (!strcmp(argv[i], "--profile"))    { profile = 1; debug = 1; }
        else if (!strcmp(argv[i], "--vmprobe"))    { vmprobe = 1; debug = 1; }
        else if (!strcmp(argv[i], "--vmexp"))      { vmexp = 1; debug = 1; }
        else if (!strcmp(argv[i], "--ctxtrace"))   { ctxtrace = 1; debug = 1; }
        /* --handload=PATH: run a HOST a.out that is not in the guest filesystem
           at all -- the emulator loads the image into a real kernel process
           instead of letting the kernel's execve do it.  That is the ONLY thing
           this mode still offers; everything else is better served by --uprog.
           `--procexp` is the old spelling and stays as a hidden alias, but it
           reads like `--procexec' and means close to the opposite, which is
           exactly why the flag was renamed. */
        else if (!strncmp(argv[i], "--handload=", 11))
            { procexp = 1; uprog_path = argv[i] + 11;
              deliver_traps = 1; trace_traps = 1; debug = 1; }
        else if (!strcmp(argv[i], "--procexp"))    { procexp = 1; debug = 1; }
        else if (!strcmp(argv[i], "--dataphys"))   dataphys = 1;
        else if (!strcmp(argv[i], "--init"))       run_init = 1;
        else if (!strncmp(argv[i], "--pwatch=", 9)) {
            char *e; pwatch_lo = (u32)strtoul(argv[i]+9, &e, 0);
            pwatch_hi = pwatch_lo + ((*e == ':') ? (u32)strtoul(e+1,0,0) : 4u);
            debug = 1;
        }
        else if (!strncmp(argv[i], "--wmem=", 7)) {
            char *e;
            wmem_lo = (u32)strtoul(argv[i] + 7, &e, 0);
            u32 len = (*e == ':') ? (u32)strtoul(e + 1, &e, 0) : 0x100u;
            wmem_hi = wmem_lo + len;
            if (*e == ':') wmem_max = strtoull(e + 1, 0, 0);
            debug = 1;
        }
        else if (!strncmp(argv[i], "--stwatch=", 10))
            { stwatch_pc = (u32)strtoul(argv[i] + 10, 0, 0); debug = 1; }
        else if (!strncmp(argv[i], "--regfind=", 10))
            { regfind_val = (u32)strtoul(argv[i] + 10, 0, 0); debug = 1; }
        else if (!strncmp(argv[i], "--traceat=", 10)) { wtrace_at = strtoull(argv[i]+10,0,0); debug = 1; }
        else if (!strncmp(argv[i], "--wtrace=", 9)) { wtrace_n = strtoull(argv[i]+9,0,0); debug = 1; }
        else if (!strncmp(argv[i], "--watch=", 8)) { watch_pc = (u32)strtoul(argv[i]+8,0,0); debug = 1; }
        else if (!strcmp(argv[i], "--trace-traps")) { deliver_traps = 1; trace_traps = 1; debug = 1; }
        else if (!strncmp(argv[i], "--tracelen=", 11)) trace_len = strtoull(argv[i]+11, 0, 0);
        else if (!strcmp(argv[i], "--no-console")) console_io = 0;
        else if (!strncmp(argv[i], "--console-port=", 15)) console_port = (int)strtoul(argv[i]+15, 0, 0);
        else if (!strcmp(argv[i], "-v"))           { verbose_sys = 1; debug = 1; }
        /* --- mode + non-option words --- */
        else if (!strcmp(argv[i], "sys"))  mode_sys = 1;
        else if (!strcmp(argv[i], "user")) mode_sys = 0;
        else if (nwords < 63) words[nwords++] = argv[i];
    }
    /* Running a user program means running it as a REAL nX process.  There used
       to be a second, SYNTHETIC model here -- the emulator serviced fork/exec/
       wait/exit itself against its own process table and address space -- and
       --uprog/--shell selected it.  It is gone: the kernel's own path does
       everything it did, including the installer.  --procexp still opts into
       the older hand-load path (the emulator loads the a.out instead of letting
       execve do it), so do not override that one. */
    if ((uprog_path || interactive) && !procexp) { procexp = 1; procexec = 1; }

    /* ★ Running a real kernel process (procexp) ALWAYS means real MC88100
       access-fault delivery and per-process u-areas.  Those were once
       independently switchable -- --hwfault/--peru with --no-hwfault/--no-peru
       to opt back out -- so the code carried `hwfault` and `peru` booleans of
       its own.  They ended up identically equal to procexp and are gone; the
       branches that used to test them test procexp directly.

       The question they existed to answer is settled and worth not re-opening:
       neither is optional.  Without per-process u-areas, curproc still names
       the parent after a fork and the first sleep panics; without real fault
       delivery the resolver has to call vm_map_pageable, which reports success
       on a copy-on-write entry without materialising anything.  Measured with
       the opt-outs still in place: --no-hwfault produced no output at all,
       --no-peru panicked from c0054c1c, and both off stopped in the fork stub.

       Turning them on for the hand-load path (--handload, procexp without
       procexec) is free -- measured instruction-identical, because that path
       pre-loads its image and so takes no user faults. */

    /* --uprog/--shell (i.e. procexec) run the kernel's own exec, which needs
       kernel memory to be coherent -- so it implies --dataphys.  It also
       defaults the machine to a genuinely single node: with 64 nodes the kernel
       puts every forked child on another node's run queue, which no CPU here
       executes.  One node keeps forks local AND boots 15x faster (3.7M
       instructions instead of 55M).  An explicit --nodes=N still wins. */
    if (procexec) {
        dataphys = 1;
        if (!cfg_nodes) cfg_nodes = 1;
    }
    /* --clock means a REAL microsecond counter (see timer_now).  It stays
       opt-in precisely so the clock-off instruction counts stay bit-exact;
       --tickdiv=N overrides the rate for experiments, including --tickdiv=0
       to get the old fast counter back with interrupts still on. */
    if (clock_irq && !tickdiv_set) tick_div = 1;
    /* One hardclock interval (10000 us, hz = 100) expressed in instructions. */
    if (!softint_period)
        softint_period = tick_div ? 10000ull * tick_div : clock_period;
    if (nwords) path = words[0];
    if (!path) {
        fprintf(stderr,
                "usage: nx88 user <binary> [args...] [-v] [--limit=N]\n"
                "       nx88 sys  <vmunix> [--limit=N] [--scsi] [--shell] [--kmsg]\n"
                "                          [--debug] [--uprog=PATH]\n"
                "                          [--handload=HOSTPATH]\n"
                "                          [--nodes=N] [--identity]\n"
                "                          [--tape=PATH] [--disk=PATH]\n"
                "                          [--console-port=N]\n"
                "  --debug      let the EMULATOR's own commentary out: the load\n"
                "               map, the synthetic boot tables, the proc\n"
                "               experiment, [kfall]/[halt]/[PANIC], the closing\n"
                "               instruction count.  Off by default, so that what\n"
                "               reaches the terminal is what the MACHINE said --\n"
                "               the kernel's log (--kmsg) and the guest's own\n"
                "               output.  Every other diagnostic flag below turns\n"
                "               this on for itself; --quiet turns it back off.\n"
                "  --kmsg       echo the kernel's own console log, prefixed [nx]\n"
                "  --tape=PATH  root filesystem image (the tape's UFS; default:\n"
                "               <vmunix-dir>.img, e.g. .../tapeimage.img)\n"
                "  --disk=PATH  SCSI sd0 install-target image (default: disk.img)\n"
                "  --hostfile=PATH  expose a host file to the guest at /hosttar\n"
                "               read-only (e.g. `tar xpf /hosttar` in the guest)\n"
                "  --shell      boot, then hand the terminal to the guest's own\n"
                "               /bin/sh, run as a real nX process\n"
                "  --console-port=N  serve the interactive console on 127.0.0.1:N\n"
                "               (VT100/telnet); the kernel log stays on stdout\n"
                "  --watch=PC   dump registers every time PC executes\n"
                "  --wtrace=N   after --watch fires, print the next N instruction\n"
                "               addresses -- answers \"which way did THIS call\n"
                "               branch\", which the aggregate --pchist cannot\n"
                "  --traceat=C  arm --wtrace at instruction count C instead\n"
                "  --wmem=VA[:len[:max]]   log writes to a VIRTUAL range\n"
                "  --pwatch=PA[:len]       log writes to a PHYSICAL range -- the\n"
                "               one that catches a bad translation scribbling\n"
                "               over memory its VA never named\n"
                "  --uprog=PATH run the guest's PATH as a REAL nX process --\n"
                "               the kernel's own execve, fork, copy-on-write,\n"
                "               faults and scheduler.  --uarg=/--uenv= supply\n"
                "               argv/envp.  Implies --dataphys and --nodes=1;\n"
                "               an explicit --nodes=N still wins.\n"
                "  --handload=HOSTPATH  run a HOST a.out that is not in the\n"
                "               guest filesystem at all: the emulator loads the\n"
                "               image into a real kernel process instead of\n"
                "               letting execve do it.  Single programs only --\n"
                "               a hand-loaded program that forks does not work.\n"
                "  --init       also let init (pid 1) run.  Off by default: it\n"
                "               is runnable from boot, and once the scheduler\n"
                "               works it forks a child that READS fd 0 -- so it\n"
                "               competes with --uprog for the console and eats\n"
                "               the script's input.\n"
                "  sys mode defaults to the real-memory model with EEPROM signature\n"
                "  'A'; pass --identity for the superseded identity path.\n");
        return 2;
    }
    /* realmm is the primary, sound path -- default it on in system mode.
       --identity selects the superseded identity+fallback path instead. */
    if (mode_sys) {
        translate_on = 1;
        ileave_stub  = 1;
        realmm = !identity_mode;
        /* Open the root filesystem image (the tape's UFS) for disk reads.
           If --tape wasn't given, derive it from the vmunix path: vmunix lives
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
        /* --shell: everything else follows.  The path is a GUEST path now --
           the kernel's execve resolves it in its own namespace. */
        if (interactive && !uprog_path) {
            uprog_path = "/bin/sh";
            if (!nuargv) uargv[nuargv++] = "sh";
            if (!nuenvp) {
                uenvp[nuenvp++] = "PATH=/bin:/etc:/usr/bin";
                uenvp[nuenvp++] = "HOME=/";
                /* vt100, not dumb: the socket console IS a VT100 and the
                   stdio one is whatever the user is sitting at.  With
                   TERM=dumb the curses games refuse to start ("Terminal
                   must have addressible cursor"), and /etc/termcap on the
                   tape has a full vt100 entry. */
                uenvp[nuenvp++] = "TERM=vt100";
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
            dbg("root image: %s (open)\n", root_img_path);
        else
            fprintf(stderr, "root image: %s could not be opened (disk reads "
                    "will return zeros) -- pass --tape=PATH\n", root_img_path);
        /* SCSI disk (sd0) target -- read/write, must already exist */
        disk_img = fopen(disk_img_path, "r+b");
        if (disk_img) {
            dbg("disk image: %s (open, rw)\n", disk_img_path);
            sd_ensure_label();
        }
        /* Host archive exposed to the guest at /hosttar (read-only). */
        if (hostfile_path) {
            hostfile_img = fopen(hostfile_path, "rb");
            if (hostfile_img) {
                fseek(hostfile_img, 0, SEEK_END);
                hostfile_size = (u32)ftell(hostfile_img);
                rewind(hostfile_img);
                /* Present a zero-padded EOF.  This /usr archive is truncated --
                   its final member (tftpd) is cut off mid-data with no tar
                   terminator -- so pad generously past real EOF: enough to let a
                   strict reader finish the truncated last member (as zeros) and
                   then see tar's two-zero-block end marker, ending at rc 0
                   instead of a checksum error.  tar stops at the terminator, so
                   the extra headroom is never actually read. */
                hostfile_vsize = ((hostfile_size + 10239u) / 10240u) * 10240u
                                 + (4u << 20);         /* + 4 MiB of zeros */
                dbg("host file: %s (open, ro, %u bytes) -> guest /hosttar\n",
                       hostfile_path, hostfile_size);
            } else {
                fprintf(stderr, "host file: %s could not be opened\n", hostfile_path);
            }
        }
        /* If asked, route the interactive console to a TCP socket instead of
           stdin/stdout so a VT100/telnet client can attach to the session. */
        console_listen(console_port);
    }
    if (mode_sys) return run_sys(path, limit, sig);

    return run_user(path, nwords, words, limit);
}
