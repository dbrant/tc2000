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
 * The socket is a serial-console stand-in, so it does the cooking a host tty
 * would: LF->CRLF on output, CR/CRLF/CR-NUL->LF on input,
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
  #include <sys/ioctl.h>
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

#ifndef __EMSCRIPTEN__                      /* no sockets in the browser build */
static consock_t con_lsock = CON_BADSOCK;   /* listening socket (--console-port) */
static consock_t con_csock = CON_BADSOCK;   /* connected client, once accepted   */
#endif
static int       con_use_sock;              /* listener is up: route console here */
static int       con_eof;                   /* client disconnected / EOF seen     */
static int       con_pushback = -1;         /* one-byte input pushback            */

#ifdef __EMSCRIPTEN__
/* ---------------------------------------------------------------------------
   ★ IN A BROWSER THE CONSOLE IS THE SOCKET ONE, not the stdio one.

   xterm.js over a postMessage pipe is the twin of the telnet client
   --console-port serves: keystrokes arrive raw and unechoed, so the whole line
   discipline -- echo, backspace, CR/CRLF -> LF, ONLCR outbound -- has to happen
   here.  Hence no third transport: this IS the socket transport with
   con_use_sock left on and only con_send_raw/con_recv_raw swapped.
   con_cook_line, the sgtty raw/cbreak switch and FIONREAD are shared unmodified,
   which is what makes the games and Emacs behave the same in a tab as telnet.

   Blocking is the other half.  A guest read() with nothing typed must WAIT, and
   a tab cannot spin -- the event loop it blocks is the one that would deliver
   the keystroke.  -sASYNCIFY unwinds the C stack back to the worker's event
   loop and rewinds it when a key lands: no thread, no SharedArrayBuffer, and so
   no COOP/COEP headers to arrange.  Cheap because the syscall dispatch sits in
   run_sys and NOT in step(), so the instrumented call graph stops short of the
   interpreter's hot loop. */
#include <emscripten.h>

/* The page supplies nxWebOut; see web/nx88-worker.js. */
EM_JS(void, nx_web_out, (const u8 *p, int n), {
    nxWebOut(HEAPU8.subarray(p, p + n));
});

#define WEBIN_CAP 8192u                     /* power of two: index is & (CAP-1) */
static u8  webin[WEBIN_CAP];
static u32 webin_r, webin_w;                /* free-running counters            */
static int webin_eof;                       /* the page closed the session      */

/* Called from JS: keystrokes, and end-of-input. */
EMSCRIPTEN_KEEPALIVE void nx_web_push(const u8 *p, int n)
{
    for (int i = 0; i < n; i++)
        if (webin_w - webin_r < WEBIN_CAP) webin[webin_w++ & (WEBIN_CAP - 1)] = p[i];
}
EMSCRIPTEN_KEEPALIVE void nx_web_eof(void) { webin_eof = 1; }

static u32 web_avail(void) { return webin_w - webin_r; }

/* Block for one typed byte.  Each nap returns to the event loop, which is the
   only thing that can ever deliver the next one. */
static int web_getc(void)
{
    while (webin_r == webin_w) {
        if (webin_eof) return -1;
        emscripten_sleep(4);
    }
    return webin[webin_r++ & (WEBIN_CAP - 1)];
}
#endif  /* __EMSCRIPTEN__ */

/* Stand up the listener.  On any failure we warn and fall back to stdio. */
void console_listen(int port)
{
#ifdef __EMSCRIPTEN__
    /* The page is the only transport there is, so take the cooked path always
       and never mind the port. */
    (void)port;
    con_use_sock = 1;
    return;
#else
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
#endif  /* __EMSCRIPTEN__ */
}

static void con_send_raw(const void *p, size_t n)
{
#ifdef __EMSCRIPTEN__
    nx_web_out((const u8 *)p, (int)n);
    return;
#else
    if (con_csock == CON_BADSOCK || con_eof) return;
    const char *b = (const char *)p;
    for (size_t off = 0; off < n; ) {
        int r = send(con_csock, b + off, (int)(n - off), 0);
        if (r <= 0) { con_eof = 1; return; }
        off += (size_t)r;
    }
#endif  /* __EMSCRIPTEN__ */
}

/* Block for the client on first console I/O, so the boot log reaches stdout
   first and the terminal shows the banner/prompt the moment it attaches. */
static void con_ensure_client(void)
{
#ifdef __EMSCRIPTEN__
    return;                                 /* the page is already attached */
#else
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
#endif  /* __EMSCRIPTEN__ */
}

static int con_recv_raw(void)
{
    if (con_pushback >= 0) { int b = con_pushback; con_pushback = -1; return b; }
#ifdef __EMSCRIPTEN__
    int b = web_getc();
    if (b < 0) { con_eof = 1; return -1; }
    return b;
#else
    u8 b;
    int r = recv(con_csock, (char *)&b, 1, 0);
    if (r <= 0) { con_eof = 1; return -1; }
    return b;
#endif
}

/* One input byte with telnet IAC sequences consumed. */
static int con_in_byte(void)
{
#ifdef __EMSCRIPTEN__
    return con_recv_raw();       /* nothing negotiates telnet on the page's wire */
#else
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
#endif  /* __EMSCRIPTEN__ */
}

/* ★ May we block waiting for a CR's paired LF/NUL?

   On a telnet wire, yes: RFC 854 encodes a line ending as CR LF or CR NUL, so
   the second byte is already coming.  Reading it unconditionally is also
   NECESSARY there -- TCP can deliver the CR alone, and an LF left unconsumed
   reads as a fresh empty line, which con_sock_read reports as EOF and which
   kills the shell.

   The page's wire has no such encoding: xterm sends one byte per keystroke, so
   RETURN is a lone CR and blocking on a byte behind it hangs the line until the
   next key is pressed.  There, peek only when a byte is already in hand -- an
   embedder's send("...\r\n") still works, both bytes reaching the ring
   together. */
#ifdef __EMSCRIPTEN__
static int con_input_ready(void);
#define CON_CR_PAIR_WAITING() con_input_ready()
#else
#define CON_CR_PAIR_WAITING() 1
#endif

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
            if (b == '\r' && CON_CR_PAIR_WAITING()) {  /* absorb a paired LF or NUL */
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
static u8  con_line[4100];      /* file scope so FIONREAD can see the leftovers */
static u32 con_line_len, con_line_pos;
static u32 con_sock_read(u8 *dst, u32 max)
{
    if (con_line_pos >= con_line_len) {               /* need a fresh line */
        con_line_len = con_line_pos = 0;
        u32 n = con_cook_line(con_line, sizeof con_line);
        if (n == 0) return 0;                         /* EOF (^D / disconnect) */
        con_line_len = n;
    }
    u32 avail = con_line_len - con_line_pos;
    u32 k = max < avail ? max : avail;
    memcpy(dst, con_line + con_line_pos, k);
    con_line_pos += k;
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

   Answering every tty ioctl with a bare "success" and reading a COOKED LINE at
   a time is enough for a shell, but a game never sends the newline that would
   end the line, so it hangs.

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
/* tchars (TIOCGETC/TIOCSETC): intr quit start stop eof brk */
static u8 tty_tc[6] = { 0x03, 0x1c, 0x11, 0x13, 0x04, 0xff };
/* ltchars (TIOCGLTC/TIOCSLTC): susp dsusp rprnt flush werase lnext.  A SEPARATE
   struct from tchars -- they were conflated here, see the ioctl switch. */
static u8 tty_ltc[6] = { 0x1a, 0x19, 0x12, 0x0f, 0x17, 0x16 };
static u32 tty_lmode;                      /* TIOCLGET/TIOCLSET local mode word */
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
        /* ★ The host must stop CHEWING THE INPUT, not just stop echoing it.
           Clearing ICANON|ECHO alone left ICRNL on, so the host tty turned the
           RETURN key's CR into LF before the emulator ever saw it -- and a guest
           that asked for CBREAK with CRMOD *clear* (Emacs sets sg_flags 0x00c2:
           CBREAK, no ECHO, no CRMOD) wants the raw CR.  Emacs binds CR to
           `newline` and LF to `C-j`, which in *scratch* is eval-print-last-sexp,
           so Enter silently evaluated instead of opening a line.
           IXON matters just as much: with it set the host swallows C-s/C-q as
           flow control, and C-s is emacs's incremental search.
           ISIG is deliberately LEFT ON so C-c still kills the emulator itself --
           the guest not seeing ^C is a known, separate gap. */
        t.c_iflag &= ~(ICRNL | INLCR | IGNCR | IXON | IXANY | ISTRIP | BRKINT);
        t.c_lflag &= ~(ICANON | ECHO | IEXTEN);
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

/* ★ How many input bytes can be had WITHOUT BLOCKING -- what FIONREAD answers.
   GNU Emacs never issues a blocking read: it polls `ioctl(0, FIONREAD)` and
   only calls read() when that reports something waiting.  With FIONREAD
   unimplemented it saw "no input" forever and ignored every keystroke while
   still redrawing perfectly, because output was never the problem.

   Exactness matters here, so this counts what WE hold as well as what the host
   has: a pushed-back byte, then whatever the transport can produce right now.
   For the socket that is a zero-timeout select, which is precise.  For stdio it
   is a select on fd 0, which can under-report while bytes sit in stdio's own
   buffer -- harmless in practice (the next keystroke re-reports them) and only
   reachable on a non-tty stdin, since a raw-mode terminal delivers a byte at a
   time.  In the browser it is exact for free: the ring IS the whole queue, and
   nothing else on the page holds a byte we have not been handed. */
/* Yield the host for a moment.  Used only when a guest poll found no input. */
static void con_idle_nap(void)
{
#if defined(__EMSCRIPTEN__)
    /* Doubly worth doing here: the nap IS the page's only chance to hand us the
       keystroke the poller is waiting for. */
    emscripten_sleep(2);
#elif !defined(_WIN32)
    struct timeval tv = { 0, 2000 };            /* 2 ms */
    select(0, 0, 0, 0, &tv);
#else
    Sleep(2);
#endif
}

#ifndef __EMSCRIPTEN__
static u32 con_std_buffered(void);
#endif
static u32 con_input_avail(void)
{
    u32 n = 0;
    if (con_pushback >= 0) n++;
    if (con_line_pos < con_line_len) n += con_line_len - con_line_pos;
    if (con_eof) return n ? n : 1;          /* EOF is "readable": read returns 0 */
#ifdef __EMSCRIPTEN__
    n += web_avail();                       /* exact: we own the whole queue */
    return (n || !webin_eof) ? n : 1;
#else
    consock_t fd;
    if (con_use_sock) {
        con_ensure_client();
        if (con_csock == CON_BADSOCK) return n;
        fd = con_csock;
    } else {
        n += con_std_buffered();          /* bytes we already pulled off fd 0 */
#ifdef _WIN32
        return n ? n : 1;   /* no cheap poll for a Windows console handle */
#else
        fd = 0;
#endif
    }
#ifdef _WIN32
    u_long cnt = 0;
    if (ioctlsocket(fd, FIONREAD, &cnt) == 0) n += (u32)cnt;
#else
    int cnt = 0;
    if (ioctl(fd, FIONREAD, &cnt) == 0 && cnt > 0) n += (u32)cnt;
#endif
    return n;
#endif  /* __EMSCRIPTEN__ */
}
static int con_input_ready(void) { return con_input_avail() != 0; }

/* ★ The stdio console reads through OUR OWN buffer, not stdio's.
   FIONREAD has to be exact, and stdio's buffer is invisible to it: one `fgetc`
   pulls a whole burst of typing off the descriptor, so the kernel then reports
   "nothing available" while the rest of the keystrokes sit in libc.  A guest
   that trusts FIONREAD -- Emacs does -- stops reading and the tail of what you
   typed is stranded until the next keypress shakes it loose.  Owning the buffer
   makes `con_input_avail` able to count it. */
#ifndef _WIN32
static u8  con_std_buf[512];
static u32 con_std_len, con_std_pos;
static int con_std_getc(void)
{
    if (con_std_pos >= con_std_len) {
        con_std_len = con_std_pos = 0;
        ssize_t r = read(0, con_std_buf, sizeof con_std_buf);
        if (r <= 0) return -1;
        con_std_len = (u32)r;
    }
    return con_std_buf[con_std_pos++];
}
#ifndef __EMSCRIPTEN__      /* the browser build counts its ring instead */
static u32 con_std_buffered(void) { return con_std_len - con_std_pos; }
#endif
#else
static int con_std_getc(void) { int c = fgetc(stdin); return c == EOF ? -1 : c; }
static u32 con_std_buffered(void) { return 0; }
#endif

/* No line editing and no echo -- the read a game in CBREAK expects.  Blocks for
   the FIRST byte, then takes whatever else is ALREADY waiting, up to `max`.
   ★ That tail matters for Emacs and nothing else: a cursor key arrives as the
   three bytes ESC [ D, and Emacs can only tell a function key from a bare ESC
   if the whole sequence comes back from one read().  Handing it one byte at a
   time left it sitting in an unresolved ESC prefix, swallowing the keys that
   followed.  Games are unaffected -- they ask for exactly 1 byte, so the drain
   loop never runs. */
/* ★ One RETURN is one keystroke.  A telnet client transmits it as CR NUL or
   CR LF (RFC 854) -- a line-ending ENCODING, not two keys -- and delivering the
   trailing byte raw handed the guest a spurious second keystroke: in Emacs's
   *scratch* that is C-@ (set-mark) or C-j (eval-print-last-sexp) after every
   Enter.  The cooked path has always absorbed it (con_cook_line); the raw path
   has to as well.  A byte that is neither is pushed back untouched. */
static int con_in_key(void)
{
    int b = con_use_sock ? con_in_byte() : con_std_getc();
    if (b < 0) return -1;
    if (b == '\r' && con_use_sock && con_input_ready()) {
        int nx = con_in_byte();
        if (nx >= 0 && nx != '\n' && nx != 0) con_pushback = nx;
    }
    return b;
}

static u32 con_read_raw(u8 *dst, u32 max)
{
    con_ensure_client();
    if (!max) return 0;
    int b = con_in_key();
    if (b < 0) return 0;
    dst[0] = (u8)b;
    u32 n = 1;
    while (n < max && con_input_ready()) {
        int c = con_in_key();
        if (c < 0) break;
        dst[n++] = (u8)c;
    }
    return n;
}

static u32 con_read_line(u8 *dst, u32 max)
{
    con_ensure_client();
    if (con_use_sock) return con_sock_read(dst, max);
    u32 n = 0;                                        /* stdio: host tty already cooks it */
    while (n < max) {
        int c = con_std_getc();
        if (c < 0) break;
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
        /* ★ Pre-fault the destination BEFORE consuming any input.  uwrite_mem
           stops at the first non-resident byte, so a read into a page the
           process has never touched transfers NOTHING and returns 0 -- which
           stdio latches as EOF -- while the line already taken off the host is
           gone.  (/usr/games/arithmetic sbrk's an 8K stdio buffer, reads into
           that untouched page, gets 0, sets _IOEOF and spins in _filbuf.)
           Order matters: fault first, and on a missing page return 1 so the
           syscall re-runs once the kernel has paged it in, input still unread.
           Faulting the whole buffer is what 4.3BSD read() does, via useracc. */
        if (n && ubuf_fault(a1, n, 1, tpc)) return 1;
        u32 got = !n ? 0
                : tty_is_raw() ? con_read_raw(tmp, n)   /* a game: one key    */
                               : con_read_line(tmp, n); /* a shell: one line  */
        got = uwrite_mem(a1, tmp, got);
        con_in_bytes += got;
        ret = (long)got;
        break;
    }
    /* ★ 4.3BSD sgtty ioctls.  The encoding is _IO?('t', cmd, size), so decode
       the low byte rather than guessing: 0x40067474 is TIOCGLTC (ltchars, cmd
       116), NOT TIOCGETC (cmd 18, 0x40067412) -- they are easily conflated.
       Any that fall through to the default arm report success WITHOUT filling
       the caller's buffer, so a program saving tty state on entry saves stack
       garbage and restores it on exit. */
    case 54:                                         /* ioctl(fd, ...)    */
        if (!fd_is_console(a0)) return 0;
        switch (a1) {
        case 0x40067408:                               /* TIOCGETP  sgttyb  */
            if (ubuf_fault(a2, 6, 1, tpc)) return 1;
            uwrite_mem(a2, tty_sg, 6);
            break;
        case 0x80067409:                               /* TIOCSETP          */
        case 0x8006740Au:                              /* TIOCSETN          */
            if (ubuf_fault(a2, 6, 0, tpc)) return 1;
            for (u32 i = 0; i < 6; i++) tty_sg[i] = mem_r8(translate(a2 + i, 0));
            host_tty_mode(tty_is_raw());
            break;
        case 0x40067412:                               /* TIOCGETC  tchars  */
            if (ubuf_fault(a2, 6, 1, tpc)) return 1;
            uwrite_mem(a2, tty_tc, 6);
            break;
        case 0x80067411u:                              /* TIOCSETC          */
            if (ubuf_fault(a2, 6, 0, tpc)) return 1;
            for (u32 i = 0; i < 6; i++) tty_tc[i] = mem_r8(translate(a2 + i, 0));
            break;
        case 0x40067474:                               /* TIOCGLTC  ltchars */
            if (ubuf_fault(a2, 6, 1, tpc)) return 1;
            uwrite_mem(a2, tty_ltc, 6);
            break;
        case 0x80067475u:                              /* TIOCSLTC          */
            if (ubuf_fault(a2, 6, 0, tpc)) return 1;
            for (u32 i = 0; i < 6; i++) tty_ltc[i] = mem_r8(translate(a2 + i, 0));
            break;
        case 0x4004747Cu: {                            /* TIOCLGET local word */
            if (ubuf_fault(a2, 4, 1, tpc)) return 1;
            u8 b[4] = { (u8)(tty_lmode >> 24), (u8)(tty_lmode >> 16),
                        (u8)(tty_lmode >> 8),  (u8)tty_lmode };
            uwrite_mem(a2, b, 4);
            break;
        }
        case 0x8004747Du:                              /* TIOCLSET          */
            if (ubuf_fault(a2, 4, 0, tpc)) return 1;
            tty_lmode = 0;
            for (u32 i = 0; i < 4; i++)
                tty_lmode = (tty_lmode << 8) | mem_r8(translate(a2 + i, 0));
            break;
        case 0x40087468u: {                            /* TIOCGWINSZ        */
            /* rows, cols, xpixels, ypixels -- halfwords.  Answering this with
               a bare success left the caller reading uninitialised stack as its
               window size; report the vt100 the termcap entry describes. */
            if (ubuf_fault(a2, 8, 1, tpc)) return 1;
            u8 b[8] = { 0, 24, 0, 80, 0, 0, 0, 0 };
            uwrite_mem(a2, b, 8);
            break;
        }
        case 0x4004667Fu: {                            /* FIONREAD          */
            /* ★ THE ONE EMACS NEEDS.  It polls this and only read()s when it
               reports something waiting, so leaving it unanswered made every
               keystroke invisible while the screen still drew perfectly. */
            if (ubuf_fault(a2, 4, 1, tpc)) return 1;
            u32 n = con_input_avail();
            u8 b[4] = { (u8)(n >> 24), (u8)(n >> 16), (u8)(n >> 8), (u8)n };
            uwrite_mem(a2, b, 4);
            /* A program that polls for input it does not have is a spin loop,
               and this one runs the host flat out.  Nothing is pending and the
               guest clock is driven by instructions, not wall time, so a short
               nap here costs the guest nothing and the host a great deal. */
            if (!n) con_idle_nap();
            break;
        }
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

   fd_console/fd_disk/fd_kernel say which fd NUMBERS the emulator answers for
   rather than the kernel.  They must be per-process: /bin/echo closes 0, 1 and
   2 on exit, and one global set would wipe the parent shell's console -- its
   next read then goes unintercepted, returns EOF, and it quits after a single
   command.

   The globals stay as the working copy every user reads directly; this saves
   and restores them around a change of current process, seeding a process
   first seen from its PARENT's set, which is exactly fork's inheritance.
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
