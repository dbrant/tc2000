/* Generated split of the former single-file nx88.c.
   Shared types, macros, hot inline accessors, and all
   cross-module declarations live in nx88.h. */
#include "nx88.h"

/* Parse the a.out header already present in a->img/a->size.  Returns 0 if it
   is a ZMAGIC image the loader understands. */
static int aout_parse(AOut *a)
{
    if (a->size < 32) return -1;
    a->magic = be32(a->img);      a->text  = be32(a->img + 4);
    a->data  = be32(a->img + 8);  a->bss   = be32(a->img + 12);
    a->syms  = be32(a->img + 16); a->entry = be32(a->img + 20);
    a->trsize = be32(a->img + 24); a->drsize = be32(a->img + 28);
    return ((a->magic & 0xFFFF) == 0x010B) ? 0 : -1;
}

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
    if (aout_parse(a)) {
        fprintf(stderr, "%s: not ZMAGIC (%08x)\n", path, a->magic);
        return -1;
    }
    return 0;
}

/* Load from an in-memory image (e.g. bytes read out of disk.img's FFS).  Takes
   ownership of `img`, which stays live as a->img the way aout_load's does. */
int aout_load_mem(u8 *img, u32 size, AOut *a)
{
    a->img = img; a->size = (long)size;
    return aout_parse(a);
}
