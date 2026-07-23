/* Generated split of the former single-file nx88.c.
   Shared types, macros, hot inline accessors, and all
   cross-module declarations live in nx88.h. */
#include "nx88.h"

void kmsg_line(const char *s, int is_log)
{
    if (is_log && !strcmp(s, kmsg_last)) return;      /* console already had it */
    printf("[nx] %s\n", s);
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
