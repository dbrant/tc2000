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
  #include <termios.h>
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


/* ---------------------------------------------------------------------------
   ★ TTY MODES -- what makes the curses games playable.

   The console used to answer every tty ioctl with a bare "success" and read
   input a COOKED LINE at a time.  That is enough for a shell, and it is why
   /usr/games/snake printed "Terminal must have addressible cursor" and then,
   once TERM was right, sat waiting for a newline that a game never sends.

   4.3BSD terminal control is sgtty, not termios.  The games do exactly this:
     TIOCGETP (0x40067408) save the current sgttyb
     TIOCGETC (0x40067474) save the current tchars
     TIOCSETP (0x80067409) set CBREAK and clear ECHO
     TIOCSETC (0x80067475) install their own interrupt/quit characters
   struct sgttyb is 6 bytes { char ispeed, ospeed, erase, kill; short flags }
   and struct tchars is 6 { intr, quit, start, stop, eof, brk }.  sg_flags:
   CBREAK 0x02, ECHO 0x08, CRMOD 0x10, RAW 0x20.

   When the guest asks for CBREAK or RAW we have to stop cooking lines AND put
   the real terminal on the other side into the same mode, or the host tty goes
   on buffering until Enter and echoing every keystroke into the game's screen.
   --------------------------------------------------------------------------- */
#define SG_CBREAK 0x02u
#define SG_ECHO   0x08u
#define SG_RAW    0x20u

static u8 tty_sg[6] = { 13, 13, 0x7f, 0x15, 0x00, SG_ECHO | 0x10 }; /* 9600,DEL,^U */
static u8 tty_tc[6] = { 0x03, 0x1c, 0x11, 0x13, 0x04, 0xff };
static u32 tty_flags(void) { return ((u32)tty_sg[4] << 8) | tty_sg[5]; }
static int tty_is_raw(void) { return (tty_flags() & (SG_CBREAK | SG_RAW)) != 0; }

#ifndef _WIN32
static struct termios host_tio_save;
static int host_tio_valid, host_tio_raw;
static void host_tty_restore(void)
{
    if (host_tio_valid && host_tio_raw) {
        tcsetattr(0, TCSANOW, &host_tio_save);
        host_tio_raw = 0;
    }
}
#endif

/* Mirror the guest's cooked/raw choice onto the host terminal.  Only for the
   stdio console: the socket console already negotiated character-at-a-time
   telnet when the client connected, so it needs nothing here. */
static void host_tty_mode(int raw)
{
#ifndef _WIN32
    if (con_use_sock || !isatty(0)) return;
    if (raw == host_tio_raw) return;
    if (!host_tio_valid) {
        if (tcgetattr(0, &host_tio_save) != 0) return;
        host_tio_valid = 1;
        atexit(host_tty_restore);
    }
    if (raw) {
        struct termios t = host_tio_save;
        t.c_lflag &= ~(ICANON | ECHO);
        t.c_cc[VMIN]  = 1;
        t.c_cc[VTIME] = 0;
        if (tcsetattr(0, TCSANOW, &t) == 0) host_tio_raw = 1;
    } else {
        tcsetattr(0, TCSANOW, &host_tio_save);
        host_tio_raw = 0;
    }
#else
    (void)raw;      /* no termios; a Windows console needs its own handling */
#endif
}

/* One byte, no line editing and no echo -- the read a game in CBREAK expects. */
static u32 con_read_raw(u8 *dst, u32 max)
{
    con_ensure_client();
    if (!max) return 0;
    if (con_use_sock) {
        int b = con_in_byte();
        if (b < 0) return 0;
        dst[0] = (u8)b;
        return 1;
    }
    int c = fgetc(stdin);
    if (c == EOF) return 0;
    dst[0] = (u8)c;
    return 1;
}

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

int fd_is_console(u32 fd)
{
    return console_io && fd < 64 && fd_console[fd];
}

/* Service raw-disk (sd0) read/write/lseek directly against emu/disk.img.
   The kernel's raw physio DMAs to the user buffer's *kernel-pmap* physical,
   which is not where the faulting process reads it, so the transferred bytes
   never arrive.  Rather than reconcile those two mappings, service the raw
   device in the emulator: read/write disk.img at the fd's byte offset straight
   into/out of the user buffer via translate().  Returns 1 if handled. */
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
        if (ubuf_fault(a1, a2, 1, tpc)) return 1;
        u8 *tmp = calloc(a2 ? a2 : 1, 1);
        if (fseek(disk_img, (long)disk_off[a0], SEEK_SET) == 0)
            if (fread(tmp, 1, a2, disk_img) == 0) { /* past EOF -> zeros */ }
        uwrite_mem(a1, tmp, a2);
        free(tmp);
        disk_off[a0] += a2;
        ret = (long)a2;
        break;
    }
    case 4: {                                          /* write(fd, buf, n) */
        if (ubuf_fault(a1, a2, 0, tpc)) return 1;
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
        if (ubuf_fault(buf, n, (flag & 0x01000000u) ? 1 : 0, tpc)) return 1;
        u8 *tmp = calloc(n ? n : 1, 1);
        if (flag & 0x01000000u) {                      /* read */
            if (fseek(disk_img, (long)blk * 512, SEEK_SET) == 0)
                if (fread(tmp, 1, n, disk_img) == 0) { /* EOF -> zeros */ }
            uwrite_mem(buf, tmp, n);
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

/* Host-file passthrough.  The guest opens the synthetic path /hosttar and the
   emulator services open/read/lseek/close straight from the host file named by
   --hostfile (read-only), so a guest tool such as `tar xpf /hosttar` can consume
   an archive that exists on no guest filesystem.  The synthesized descriptor is
   allocated high (>=40) so it never collides with the low fds the kernel hands
   out for tar's output files.  Returns 1 if handled. */
#define HOSTFILE_GUESTPATH "/hosttar"
int hostfile_syscall(u32 sysno, u32 tpc)
{
    if (!hostfile_img) return 0;
    u32 a0 = RD(2), a1 = RD(3), a2 = RD(4);
    long ret;
    switch (sysno) {
    case 5: {                                          /* open(path, flags, mode) */
        char p[128];
        uread_str(a0, p, sizeof p);
        if (strcmp(p, HOSTFILE_GUESTPATH) != 0) return 0;   /* not our path */
        int fd = -1;
        for (int i = 40; i < 64; i++) if (!fd_host[i]) { fd = i; break; }
        if (fd < 0) return 0;
        fd_host[fd] = 1; host_off[fd] = 0;
        ret = fd;
        break;
    }
    case 3: {                                          /* read(fd, buf, n) */
        if (a0 >= 64 || !fd_host[a0]) return 0;
        u32 off = host_off[a0];
        if (off >= hostfile_vsize) { ret = 0; break; } /* true EOF (past padding) */
        u32 want = a2;
        if (off + want > hostfile_vsize) want = hostfile_vsize - off;
        if (ubuf_fault(a1, want, 1, tpc)) return 1;
        u8 *tmp = calloc(want ? want : 1, 1);          /* zero-filled */
        if (off < hostfile_size) {                     /* real bytes, then zeros */
            u32 rn = (off + want <= hostfile_size) ? want : (hostfile_size - off);
            if (fseek(hostfile_img, (long)off, SEEK_SET) == 0)
                if (fread(tmp, 1, rn, hostfile_img) == 0) { /* short: leave zeros */ }
        }
        uwrite_mem(a1, tmp, want);
        free(tmp);
        host_off[a0] += want;
        ret = (long)want;
        break;
    }
    case 19: {                                         /* lseek(fd, off, whence) */
        if (a0 >= 64 || !fd_host[a0]) return 0;
        if (a2 == 0)      host_off[a0] = a1;               /* SEEK_SET */
        else if (a2 == 1) host_off[a0] += a1;              /* SEEK_CUR */
        else if (a2 == 2) host_off[a0] = hostfile_vsize + a1;/* SEEK_END */
        ret = (long)host_off[a0];
        break;
    }
    case 6:                                            /* close(fd) */
        if (a0 >= 64 || !fd_host[a0]) return 0;
        fd_host[a0] = 0; host_off[a0] = 0;
        ret = 0;
        break;
    default:
        return 0;
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
    switch (sysno) {
    case 4: {                                          /* write(fd, buf, n) */
        if (!fd_is_console(a0)) return 0;
        u8 tmp[4096];
        u32 total = a2 < (1u << 20) ? a2 : (1u << 20);
        if (ubuf_fault(a1, total, 0, tpc)) return 1;
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
        /* ★ Pre-fault the destination BEFORE consuming any input.  This was the
           one emulator-serviced buffer in this file that did not, and it lost
           data as well as short-reading: `uwrite_mem` stops at the first
           non-resident byte, so a read into a page the process has never
           touched transferred NOTHING and returned 0 -- which stdio latches as
           EOF -- while the line it had already taken off the host was gone.
           /usr/games/arithmetic died on exactly that: it sbrk's an 8K stdio
           buffer at 0xc000, asks its first question, does
           read(0, 0xc000, 8192) into that untouched page, gets 0 back, sets
           _IOEOF and then spins forever in _filbuf (0x5bc-0x628 -> 0xb48)
           without ever issuing another syscall.  The answer you typed was
           swallowed and nothing happened.
           The order matters: fault first, and if a page is missing return 1 so
           the whole syscall re-runs once the kernel has paged it in -- with the
           input still unread.  Faulting the caller's whole buffer is also what
           the real 4.3BSD read() does (useracc over the full length). */
        if (n && ubuf_fault(a1, n, 1, tpc)) return 1;
        u32 got = !n ? 0
                : tty_is_raw() ? con_read_raw(tmp, n)   /* a game: one key    */
                               : con_read_line(tmp, n); /* a shell: one line  */
        got = uwrite_mem(a1, tmp, got);
        con_in_bytes += got;
        ret = (long)got;
        break;
    }
    case 54:                                         /* ioctl(fd, ...)    */
        if (!fd_is_console(a0)) return 0;
        switch (a1) {
        case 0x40067408:                               /* TIOCGETP sgttyb   */
            if (ubuf_fault(a2, 6, 1, tpc)) return 1;
            uwrite_mem(a2, tty_sg, 6);
            break;
        case 0x80067409:                               /* TIOCSETP          */
        case 0x8006740Au:                              /* TIOCSETN          */
            if (ubuf_fault(a2, 6, 0, tpc)) return 1;
            for (u32 i = 0; i < 6; i++) tty_sg[i] = mem_r8(translate(a2 + i, 0));
            host_tty_mode(tty_is_raw());
            break;
        case 0x40067474:                               /* TIOCGETC tchars   */
            if (ubuf_fault(a2, 6, 1, tpc)) return 1;
            uwrite_mem(a2, tty_tc, 6);
            break;
        case 0x80067475:                               /* TIOCSETC          */
            if (ubuf_fault(a2, 6, 0, tpc)) return 1;
            for (u32 i = 0; i < 6; i++) tty_tc[i] = mem_r8(translate(a2 + i, 0));
            break;
        default: break;                                /* yes, it's a tty   */
        }
        ret = 0;
        break;
    default:
        return 0;
    }
    WR(2, (u32)ret);
    cpu.pc = tpc + 8;                                  /* success return    */
    return 1;
}

/* ---------------------------------------------------------------------------
   ★ PER-PROCESS descriptor flags.

   fd_console/fd_disk/fd_kernel say which fd NUMBERS the emulator
   answers for rather than the kernel.  They were one global set, which was
   fine while only one process existed.  With real fork/exec they have to be
   per-process: /bin/echo closes 0, 1 and 2 in its exit path, and that wiped
   the parent shell's console -- so the shell's next read was no longer
   intercepted, returned EOF, and it quit after a single command.

   The globals stay as the working copy (every existing user reads them
   directly and stays unchanged); this just saves and restores them around a
   change of current process, seeding a process first seen from its PARENT's
   set, which is exactly fork's inheritance.  Only where there
   are real processes to tell apart.
   --------------------------------------------------------------------------- */
#define FDSETS 24
typedef struct {
    int  pid, used;
    u8   con[64], dsk[64], krn[64];
    u32  doff[64];
} FdSet;
static FdSet fdsets[FDSETS];
static int fdcur = -1;

static FdSet *fdslot(int pid)
{
    for (int i = 0; i < FDSETS; i++)
        if (fdsets[i].used && fdsets[i].pid == pid) return &fdsets[i];
    for (int i = 0; i < FDSETS; i++)
        if (!fdsets[i].used) { fdsets[i].used = 1; fdsets[i].pid = pid; return &fdsets[i]; }
    return 0;                                  /* table full: keep the globals */
}

static void fd_save(FdSet *s)
{
    memcpy(s->con, fd_console, 64);
    memcpy(s->dsk, fd_disk, 64);    memcpy(s->krn, fd_kernel, 64);
    memcpy(s->doff, disk_off, sizeof disk_off);
}

static void fd_load(const FdSet *s)
{
    memcpy(fd_console, s->con, 64);
    memcpy(fd_disk, s->dsk, 64);    memcpy(fd_kernel, s->krn, 64);
    memcpy(disk_off, s->doff, sizeof disk_off);
}

void fd_switch(int pid, int ppid)
{
    if (!procexp || pid <= 0 || pid == fdcur) return;
    if (fdcur > 0) { FdSet *o = fdslot(fdcur); if (o) fd_save(o); }
    FdSet *n = fdslot(pid);
    if (!n) { fdcur = pid; return; }
    if (!n->con[0] && !n->con[1] && !n->con[2] && !n->krn[0]) {
        /* first sight: inherit from the parent, or from whatever is live now */
        FdSet *p = (ppid > 0) ? fdslot(ppid) : 0;
        if (p && (p->con[0] || p->con[1] || p->con[2] || p->krn[0])) fd_load(p);
        fd_save(n);
    }
    fd_load(n);
    fdcur = pid;
}
