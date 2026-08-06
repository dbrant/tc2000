/* Generated split of the former single-file nx88.c.
   Shared types, macros, hot inline accessors, and all
   cross-module declarations live in nx88.h. */
#include "nx88.h"

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
        /* --- run configuration --- */
        if      (!strncmp(argv[i], "--limit=", 8)) limit = strtoull(argv[i] + 8, 0, 0);
        else if (!strncmp(argv[i], "--sig=", 6))   sig = (u32)(u8)argv[i][6];
        else if (!strncmp(argv[i], "--nodes=", 8)) cfg_nodes = (u32)strtoul(argv[i]+8,0,0);
        else if (!strncmp(argv[i], "--root=", 7))  root_img_path = argv[i]+7;
        else if (!strcmp(argv[i], "--scsi"))       sha_desc_clear = 0x14;
        else if (!strcmp(argv[i], "--identity"))   identity_mode = 1;
        else if (!strcmp(argv[i], "--clock"))      clock_irq = 1;
        else if (!strncmp(argv[i], "--clock=", 8)) { clock_irq = 1; clock_period = strtoull(argv[i]+8,0,0); }
        /* --- user-mode program launch --- */
        else if (!strcmp(argv[i], "--shell")) {     /* boot, then hand off /bin/sh */
            interactive = 1; utest = 1; deliver_traps = 1;
            trace_traps = 0; quiet_uproc = 1;
            limit = ~0ull;
        }
        else if (!strcmp(argv[i], "--utest"))          { utest = 1; deliver_traps = 1; trace_traps = 1; }
        else if (!strncmp(argv[i], "--uprog=", 8))     { uprog_path = argv[i]+8; utest = 1; deliver_traps = 1; trace_traps = 1; }
        else if (!strncmp(argv[i], "--uarg=", 7) && nuargv < MAX_UARGV) uargv[nuargv++] = argv[i] + 7;
        else if (!strncmp(argv[i], "--uenv=", 7) && nuenvp < MAX_UENVP) uenvp[nuenvp++] = argv[i] + 7;
        /* --- output / diagnostics --- */
        else if (!strcmp(argv[i], "--quiet"))      { trace_traps = 0; quiet_uproc = 1; }
        else if (!strcmp(argv[i], "--kmsg"))       kmsgs = 1;
        else if (!strcmp(argv[i], "--scsitrace"))  scsi_trace = 1;
        else if (!strcmp(argv[i], "--pchist"))     dump_pchist = 1;
        else if (!strncmp(argv[i], "--watch=", 8)) watch_pc = (u32)strtoul(argv[i]+8,0,0);
        else if (!strcmp(argv[i], "--trace-traps")) { deliver_traps = 1; trace_traps = 1; }
        else if (!strncmp(argv[i], "--tracelen=", 11)) trace_len = strtoull(argv[i]+11, 0, 0);
        else if (!strcmp(argv[i], "--no-console")) console_io = 0;
        else if (!strcmp(argv[i], "-v"))           verbose_sys = 1;
        /* --- mode + non-option words --- */
        else if (!strcmp(argv[i], "sys"))  mode_sys = 1;
        else if (!strcmp(argv[i], "user")) mode_sys = 0;
        else if (nwords < 63) words[nwords++] = argv[i];
    }
    if (nwords) path = words[0];
    if (!path) {
        fprintf(stderr,
                "usage: nx88 user <binary> [args...] [-v] [--limit=N]\n"
                "       nx88 sys  <vmunix> [--limit=N] [--scsi] [--shell] [--kmsg]\n"
                "                          [--nodes=N] [--identity]\n"
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
        /* SCSI disk (sd0) target -- read/write, must already exist */
        disk_img = fopen(disk_img_path, "r+b");
        if (disk_img) {
            printf("disk image: %s (open, rw)\n", disk_img_path);
            sd_ensure_label();
        }
    }
    if (mode_sys) return run_sys(path, limit, sig);

    return run_user(path, nwords, words, limit);
}
