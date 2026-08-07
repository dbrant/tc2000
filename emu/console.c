/* Generated split of the former single-file nx88.c.
   Shared types, macros, hot inline accessors, and all
   cross-module declarations live in nx88.h. */
#include "nx88.h"

/* --------------------------------------------------------------------------
 * Interactive console transport.
 *
 * By default the guest's console (the /bin/sh session under --shell) reads and
 * writes the emulator's own stdin/stdout, sharing them with the kernel log.
 * With --console-port=N the emulator instead listens on a loopback TCP socket:
 * the kernel boot log keeps flowing to stdout, and a VT100/telnet client that
 * connects to the port gets the interactive session on its own wire.
 *
 * The socket is a serial-console stand-in, so it does the terminal cooking the
 * host tty used to do for us: LF->CRLF on output, CR/CRLF/CR-NUL->LF on input,
 * server-side echo with backspace editing, and ^D as end-of-file.  Minimal
 * telnet IAC negotiation puts a standard telnet client into character mode;
 * raw-TCP clients (nc, PuTTY "Raw") ignore it and work the same.
 * ------------------------------------------------------------------------ */
#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  typedef SOCKET consock_t;
  #define CON_BADSOCK   INVALID_SOCKET
  #define con_closesock closesocket
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  typedef int consock_t;
  #define CON_BADSOCK   (-1)
  #define con_closesock close
#endif
#ifndef TCP_NODELAY
  #define TCP_NODELAY 1
#endif

static consock_t con_lsock = CON_BADSOCK;   /* listening socket (--console-port) */
static consock_t con_csock = CON_BADSOCK;   /* connected client, once accepted   */
static int       con_use_sock;              /* listener is up: route console here */
static int       con_eof;                   /* client disconnected / EOF seen     */
static int       con_pushback = -1;         /* one-byte input pushback            */

/* Stand up the listener.  On any failure we warn and fall back to stdio. */
void console_listen(int port)
{
    if (port <= 0) return;
    console_port = port;
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "console: WSAStartup failed; using stdio\n");
        return;
    }
#endif
    con_lsock = socket(AF_INET, SOCK_STREAM, 0);
    if (con_lsock == CON_BADSOCK) {
        fprintf(stderr, "console: socket() failed; using stdio\n");
        return;
    }
    int one = 1;
    setsockopt(con_lsock, SOL_SOCKET, SO_REUSEADDR, (char *)&one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family      = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   /* loopback only, not the network */
    a.sin_port        = htons((u16)port);
    if (bind(con_lsock, (struct sockaddr *)&a, sizeof a) != 0 ||
        listen(con_lsock, 1) != 0) {
        fprintf(stderr, "console: bind/listen on port %d failed; using stdio\n", port);
        con_closesock(con_lsock);
        con_lsock = CON_BADSOCK;
        return;
    }
    con_use_sock = 1;
    printf("console: listening on 127.0.0.1:%d -- connect a VT100/telnet client "
           "there for the interactive session (kernel log stays on stdout)\n", port);
    fflush(stdout);
}

static void con_send_raw(const void *p, size_t n)
{
    if (con_csock == CON_BADSOCK || con_eof) return;
    const char *b = (const char *)p;
    for (size_t off = 0; off < n; ) {
        int r = send(con_csock, b + off, (int)(n - off), 0);
        if (r <= 0) { con_eof = 1; return; }
        off += (size_t)r;
    }
}

/* Block for the client on first console I/O, so the boot log reaches stdout
   first and the terminal shows the banner/prompt the moment it attaches. */
static void con_ensure_client(void)
{
    if (!con_use_sock || con_csock != CON_BADSOCK || con_eof) return;
    printf("console: waiting for a connection on port %d ...\n", console_port);
    fflush(stdout);
    struct sockaddr_in cli;
    socklen_t cl = sizeof cli;
    con_csock = accept(con_lsock, (struct sockaddr *)&cli, &cl);
    if (con_csock == CON_BADSOCK) { con_eof = 1; return; }
    int one = 1;
    setsockopt(con_csock, IPPROTO_TCP, TCP_NODELAY, (char *)&one, sizeof one);
    /* IAC WILL ECHO, IAC WILL SGA, IAC DO SGA: character-at-a-time, we echo. */
    static const u8 nego[] = { 255,251,1, 255,251,3, 255,253,3 };
    con_send_raw(nego, sizeof nego);
    printf("console: client connected on port %d.\n", console_port);
    fflush(stdout);
}

static int con_recv_raw(void)
{
    if (con_pushback >= 0) { int b = con_pushback; con_pushback = -1; return b; }
    u8 b;
    int r = recv(con_csock, (char *)&b, 1, 0);
    if (r <= 0) { con_eof = 1; return -1; }
    return b;
}

/* One input byte with telnet IAC sequences consumed. */
static int con_in_byte(void)
{
    for (;;) {
        int b = con_recv_raw();
        if (b < 0) return -1;
        if (b != 255) return b;                       /* not IAC */
        int c = con_recv_raw();
        if (c < 0) return -1;
        if (c == 255) return 255;                     /* escaped literal 0xff */
        if (c >= 251 && c <= 254) { con_recv_raw(); continue; }  /* WILL/WONT/DO/DONT + opt */
        if (c == 250) {                               /* SB ... IAC SE */
            for (;;) {
                int d = con_recv_raw();
                if (d < 0) return -1;
                if (d == 255) {
                    int e = con_recv_raw();
                    if (e < 0) return -1;
                    if (e == 240) break;              /* SE */
                }
            }
            continue;
        }
        /* other 2-byte IAC commands (NOP, DM, ...): swallow */
    }
}

/* Assemble one cooked line from the socket: server-side echo, backspace editing,
   CR / CRLF / CR-NUL -> LF, ^D as EOF.  Returns length; 0 means end-of-file
   (client closed or ^D on an empty line). */
static u32 con_cook_line(u8 *dst, u32 cap)
{
    u32 n = 0;
    for (;;) {
        int b = con_in_byte();
        if (b < 0) return n;                          /* disconnect: 0 => EOF */
        if (b == '\r' || b == '\n') {
            if (b == '\r') {                          /* absorb a paired LF or NUL */
                int nx = con_in_byte();
                if (nx >= 0 && nx != '\n' && nx != 0) con_pushback = nx;
            }
            con_send_raw("\r\n", 2);
            if (n < cap) dst[n++] = '\n';
            return n;
        }
        if (b == 0) continue;                         /* stray NUL */
        if (b == 0x7f || b == 0x08) {                 /* DEL / backspace */
            if (n > 0) { n--; con_send_raw("\b \b", 3); }
            continue;
        }
        if (b == 4) return n;                         /* ^D: EOF if empty, else flush line */
        if (n < cap) {
            dst[n++] = (u8)b;
            if (b >= 32 || b == '\t') { u8 e = (u8)b; con_send_raw(&e, 1); }  /* echo */
        }
    }
}

/* Serve a guest read() from the socket.  The tty line discipline the host used
   to give us for stdin lives here: a whole cooked line is buffered, then drained
   across as many guest reads as it takes -- the 1989 /bin/sh reads a byte at a
   time, so a line must survive many one-byte reads. */
static u32 con_sock_read(u8 *dst, u32 max)
{
    static u8  line[4100];
    static u32 len, pos;
    if (pos >= len) {                                 /* need a fresh line */
        len = pos = 0;
        u32 n = con_cook_line(line, sizeof line);
        if (n == 0) return 0;                         /* EOF (^D / disconnect) */
        len = n;
    }
    u32 avail = len - pos, k = max < avail ? max : avail;
    memcpy(dst, line + pos, k);
    pos += k;
    return k;
}

static void con_sock_write(const u8 *p, size_t n)
{
    u8 buf[2048];
    size_t j = 0;
    for (size_t i = 0; i < n; i++) {
        if (p[i] == '\n') { buf[j++] = '\r'; buf[j++] = '\n'; }  /* ONLCR */
        else buf[j++] = p[i];
        if (j >= sizeof buf - 2) { con_send_raw(buf, j); j = 0; }
    }
    if (j) con_send_raw(buf, j);
}

/* Public transport: used by console_syscall (writes/reads) and the shell
   banner in proc.c.  Falls back to stdio/stderr when no socket is configured. */
static void con_write(const u8 *p, size_t n, int is_err)
{
    con_ensure_client();
    if (con_use_sock) { con_sock_write(p, n); return; }
    FILE *f = is_err ? stderr : stdout;
    fwrite(p, 1, n, f);
    fflush(f);
}

void con_write_str(const char *s) { con_write((const u8 *)s, strlen(s), 0); }

static u32 con_read_line(u8 *dst, u32 max)
{
    con_ensure_client();
    if (con_use_sock) return con_sock_read(dst, max);
    u32 n = 0;                                        /* stdio: host tty already cooks it */
    while (n < max) {
        int c = fgetc(stdin);
        if (c == EOF) break;
        dst[n++] = (u8)c;
        if (c == '\n') break;
    }
    return n;
}

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
    case 4: {                                          /* write(fd, buf, n) */
        if (!fd_is_console(a0)) return 0;
        u8 tmp[4096];
        u32 total = a2 < (1u << 20) ? a2 : (1u << 20);
        for (u32 off = 0; off < total; ) {
            u32 chunk = total - off;
            if (chunk > sizeof tmp) chunk = sizeof tmp;
            for (u32 i = 0; i < chunk; i++) tmp[i] = mem_r8(translate(a1 + off + i, 0));
            con_write(tmp, chunk, a0 == 2);
            off += chunk;
        }
        con_out_bytes += a2;
        ret = (long)a2;
        break;
    }
    case 3: {                                          /* read(fd, buf, n)  */
        if (!fd_is_console(a0)) return 0;
        u32 n = a2 > 4096 ? 4096 : a2;
        u8 tmp[4096];
        u32 got = n ? con_read_line(tmp, n) : 0;        /* one cooked line   */
        for (u32 i = 0; i < got; i++) mem_w8(translate(a1 + i, 0), tmp[i]);
        con_in_bytes += got;
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
