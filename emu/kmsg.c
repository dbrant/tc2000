/* Generated split of the former single-file nx88.c.
   Shared types, macros, hot inline accessors, and all
   cross-module declarations live in nx88.h. */
#include "nx88.h"
#include <ctype.h>

/* Case-insensitive substring search.  strcasestr is neither C nor available on
   every toolchain this builds with, and the haystacks here are one short line. */
static int has_ci(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < n && hay[i] &&
               tolower((unsigned char)hay[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == n) return 1;
    }
    return 0;
}

/* Which lines get which colour.  These are the SAME three rules the page
   applies (`highlight' in web/tc2000.js) and are meant to stay in step, so that
   a boot log read in a terminal and one read in a browser look alike:

     the machine's identity     bold bright white
     something went wrong       bold amber -- the tape's probes find plenty
     the kernel's log() lines    dim -- allocator sizes, internal bookkeeping

   `s' arrives WITHOUT the "[nx] " prefix, so the priority lines are tested as a
   leading "<6>" rather than the page's /^\[nx\] <\d>/.  First match wins, and
   TC2000 is checked first for parity with the page. */
static const char *kmsg_sgr(const char *s)
{
    if (strstr(s, "TC2000")) return "1;97";
    if (has_ci(s, "panic") || has_ci(s, "warning") ||
        has_ci(s, "error") || has_ci(s, "failed")) return "1;33";
    if (s[0] == '<' && isdigit((unsigned char)s[1]) && s[2] == '>') return "2";
    return 0;
}

void kmsg_line(const char *s, int is_log)
{
    if (is_log && !strcmp(s, kmsg_last)) return;      /* console already had it */
    /* Colour is resolved once in main.c: on for a terminal, off when this is
       redirected into a file, and never on by default in the browser build --
       there the PAGE does the styling and a second set of escapes would fight
       it.  The guest's own output is not touched here or anywhere. */
    const char *sgr = kmsg_color ? kmsg_sgr(s) : 0;
    if (sgr) printf("\033[%sm[nx] %s\033[0m\n", sgr, s);
    else     printf("[nx] %s\n", s);
    fflush(stdout);
    if (!is_log) { strncpy(kmsg_last, s, KMSG_MAX - 1); kmsg_last[KMSG_MAX-1] = 0; }
}

void kmsg_putchar(int c, u32 flags)
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

void kmsg_flush(void)
{
    if (kmsg_conslen) { kmsg_cons[kmsg_conslen] = 0; kmsg_line(kmsg_cons, 0); kmsg_conslen = 0; }
    if (kmsg_loglen)  { kmsg_log[kmsg_loglen]  = 0; kmsg_line(kmsg_log, 1);  kmsg_loglen  = 0; }
}
