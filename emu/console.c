/* Generated split of the former single-file nx88.c.
   Shared types, macros, hot inline accessors, and all
   cross-module declarations live in nx88.h. */
#include "nx88.h"

int pipe_alloc(void)
{
    for (int i = 0; i < MAX_PIPES; i++)
        if (!pipes[i].used) {
            pipes[i].used = 1; pipes[i].len = pipes[i].rpos = 0;
            if (!pipes[i].buf) { pipes[i].cap = 65536; pipes[i].buf = malloc(pipes[i].cap); }
            return i;
        }
    return -1;
}

void pipe_write(Pipe *p, const u8 *src, u32 n)
{
    if (p->len + n > p->cap) {
        while (p->len + n > p->cap) p->cap *= 2;
        p->buf = realloc(p->buf, p->cap);
    }
    memcpy(p->buf + p->len, src, n);
    p->len += n;
}

int fd_is_console(u32 fd)
{
    return console_io && fd < 64 && fd_console[fd];
}

/* Service raw-disk (sd0) read/write/lseek directly against emu/disk.img.
   The kernel's raw physio DMAs to the user buffer's *kernel-pmap* physical, but
   our synthetic user process reads through its own usegtab (a different physical
   page), so the transferred bytes never reach it.  Rather than reconcile those
   two mappings, service the raw device in the emulator: read/write disk.img at
   the fd's byte offset straight into/out of the user buffer via translate() --
   the same usegtab the process itself uses.  Returns 1 if handled. */
int disk_syscall(u32 sysno, u32 tpc)
{
    u32 a0 = RD(2), a1 = RD(3), a2 = RD(4);
    if (a0 >= 64 || !fd_disk[a0] || !disk_img) return 0;
    long ret;
    switch (sysno) {
    case 19: {                                         /* lseek(fd, off, whence) */
        long end; fseek(disk_img, 0, SEEK_END); end = ftell(disk_img);
        if (a2 == 0)      disk_off[a0] = a1;           /* SEEK_SET */
        else if (a2 == 1) disk_off[a0] += a1;          /* SEEK_CUR */
        else if (a2 == 2) disk_off[a0] = (u32)end + a1;/* SEEK_END */
        ret = (long)disk_off[a0];
        break;
    }
    case 3: {                                          /* read(fd, buf, n) */
        u8 *tmp = calloc(a2 ? a2 : 1, 1);
        if (fseek(disk_img, (long)disk_off[a0], SEEK_SET) == 0)
            if (fread(tmp, 1, a2, disk_img) == 0) { /* past EOF -> zeros */ }
        for (u32 i = 0; i < a2; i++) mem_w8(translate(a1 + i, 0), tmp[i]);
        free(tmp);
        disk_off[a0] += a2;
        ret = (long)a2;
        break;
    }
    case 4: {                                          /* write(fd, buf, n) */
        u8 *tmp = malloc(a2 ? a2 : 1);
        for (u32 i = 0; i < a2; i++) tmp[i] = mem_r8(translate(a1 + i, 0));
        if (fseek(disk_img, (long)disk_off[a0], SEEK_SET) == 0)
            fwrite(tmp, 1, a2, disk_img), fflush(disk_img);
        free(tmp);
        disk_off[a0] += a2;
        ret = (long)a2;
        break;
    }
    case 54: {                                         /* ioctl */
        /* newfs' read_label / raw block I/O ioctl (0xc014000d): a 20-byte
           request {u32 block@0, u32 count@8 (512-byte blocks), u32 flags@0xc
           (bit 0x01000000 => read), u32 buf@0x10}.  The kernel would physio
           this to the user buffer's pmap-physical; service it ourselves. */
        if (a1 != 0xC014000Du) return 0;               /* other ioctls to kernel */
        u32 blk  = mem_r32(translate(a2 + 0x00, 0));
        u32 cnt  = mem_r32(translate(a2 + 0x08, 0));
        u32 flag = mem_r32(translate(a2 + 0x0c, 0));
        u32 buf  = mem_r32(translate(a2 + 0x10, 0));
        u32 n = cnt * 512;
        u8 *tmp = calloc(n ? n : 1, 1);
        if (flag & 0x01000000u) {                      /* read */
            if (fseek(disk_img, (long)blk * 512, SEEK_SET) == 0)
                if (fread(tmp, 1, n, disk_img) == 0) { /* EOF -> zeros */ }
            for (u32 i = 0; i < n; i++) mem_w8(translate(buf + i, 0), tmp[i]);
        } else {                                       /* write */
            for (u32 i = 0; i < n; i++) tmp[i] = mem_r8(translate(buf + i, 0));
            if (fseek(disk_img, (long)blk * 512, SEEK_SET) == 0)
                fwrite(tmp, 1, n, disk_img), fflush(disk_img);
        }
        free(tmp);
        ret = 0;
        break;
    }
    default:
        return 0;                                      /* close, etc.: to kernel */
    }
    WR(2, (u32)ret);
    cpu.pc = tpc + 8;
    return 1;
}

/* Service a syscall entirely in the emulator.  Returns 1 if handled, and
   leaves the result in r2 with the pc advanced past the error branch. */
int console_syscall(u32 sysno, u32 tpc)
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
