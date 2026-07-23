/* Generated split of the former single-file nx88.c.
   Shared types, macros, hot inline accessors, and all
   cross-module declarations live in nx88.h. */
#include "nx88.h"

void mem_load(u32 a, const u8 *src, size_t n)
{
    for (size_t i = 0; i < n; i++) mem_w8(a + (u32)i, src[i]);
}

void mem_zero(u32 a, size_t n)
{
    for (size_t i = 0; i < n; i++) mem_w8(a + (u32)i, 0);
}

int mem_cstr(u32 a, char *out, int max)
{
    int i = 0;
    while (i < max - 1) { u8 c = mem_r8(a + i); if (!c) break; out[i++] = (char)c; }
    out[i] = 0;
    return i;
}

u8 *cmram_page_of(u32 sel)
{
    u32 i = (sel >> 12) & (NPAGES - 1);
    if (!cmram_pages[i]) cmram_pages[i] = calloc(1, PAGE_SIZE);
    return cmram_pages[i];
}

u32 cmram_r32(u32 sel)
{
    u8 *p = cmram_page_of(sel) + (sel & (PAGE_SIZE - 4));
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

void cmram_w32(u32 sel, u32 v)
{
    u8 *p = cmram_page_of(sel) + (sel & (PAGE_SIZE - 4));
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}
