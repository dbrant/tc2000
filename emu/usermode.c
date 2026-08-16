/* Generated split of the former single-file nx88.c.
   Shared types, macros, hot inline accessors, and all
   cross-module declarations live in nx88.h. */
#include "nx88.h"

void do_syscall(void)
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

u32 build_stack(int argc, char **argv)
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

int run_user(const char *path, int argc, char **argv, u64 limit)
{
    AOut a;
    if (aout_load(path, &a)) return 1;
    mem_load(0, a.img + HDR_PAGE, a.text);
    u32 dv = (a.text + 4095) & ~4095u;
    mem_load(dv, a.img + HDR_PAGE + a.text, a.data);
    mem_zero(dv + a.data, a.bss);

    dbg("%s\n  text %u @ 00000000  data %u @ %08x  bss %u  entry %08x\n",
           path, a.text, a.data, dv, a.bss, a.entry);

    memset(&cpu, 0, sizeof cpu);
    cpu.pc = a.entry;
    WR(31, build_stack(argc, argv));

    dbg("--- output ------------------------------------------------\n");
    while (cpu.count < limit) {
        if (step()) {
            if (trap_vector == 128) {
                do_syscall();
                if (exited >= 0) {
                    dbg("\n-----------------------------------------------------------\n");
                    dbg("exited(%d) after %llu instructions\n",
                           exited, (unsigned long long)cpu.count);
                    return 0;
                }
                /* success skips the error branch: resume at pc+8 */
                cpu.pc = trap_pc + (sys_err ? 4 : 8);
                trap_taken = 0;
            } else {
                dbg("\n*** trap %d at pc=%08x after %llu instructions\n",
                       (int)trap_vector, trap_pc, (unsigned long long)cpu.count);
                return 1;
            }
        }
    }
    dbg("\n(instruction limit reached at pc=%08x)\n", cpu.pc);
    return 0;
}
