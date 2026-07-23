/* Generated split of the former single-file nx88.c.
   Shared types, macros, hot inline accessors, and all
   cross-module declarations live in nx88.h. */
#include "nx88.h"

int aout_load(const char *path, AOut *a)
{
    FILE *f = fopen(path, "rb");
    /* A miss is routine once a shell is running -- it is just PATH being
       searched -- so let the caller report it in the guest's own terms. */
    if (!f) { if (!quiet_uproc) perror(path); return -1; }
    fseek(f, 0, SEEK_END); a->size = ftell(f); fseek(f, 0, SEEK_SET);
    a->img = malloc(a->size);
    if (fread(a->img, 1, a->size, f) != (size_t)a->size) { fclose(f); return -1; }
    fclose(f);
    a->magic = be32(a->img);      a->text  = be32(a->img + 4);
    a->data  = be32(a->img + 8);  a->bss   = be32(a->img + 12);
    a->syms  = be32(a->img + 16); a->entry = be32(a->img + 20);
    a->trsize = be32(a->img + 24); a->drsize = be32(a->img + 28);
    if ((a->magic & 0xFFFF) != 0x010B) {
        fprintf(stderr, "%s: not ZMAGIC (%08x)\n", path, a->magic);
        return -1;
    }
    return 0;
}
