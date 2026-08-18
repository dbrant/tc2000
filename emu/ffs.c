/* Read-only 4.3BSD FFS reader, over any image the host can open.
 *
 * The boot image is a filesystem that CONTAINS THE KERNEL, and this is what
 * reads it out (see main.c), so the emulator needs one file and nothing else.
 * It is also the only way to browse an image from the host.
 *
 * ffs_read_file() takes only REGULAR files; directories return -1 by design.
 *
 * Superblock -> name lookup -> inode -> data blocks.  Big-endian; the on-disk
 * field offsets are pinned against a disk.img the guest itself newfs'd, and
 * read BBN's 1989 tape image unchanged -- same newfs, same layout.
 * Read-only: no allocation, no bitmap or cylinder-group summary maintenance. */
#include "nx88.h"

#define FFS_SBOFF        8192      /* superblock byte offset (BBSIZE)          */
#define FFS_MAGIC        0x011954u
#define FFS_ROOTINO      2
#define FFS_DINODE_SIZE  128

typedef struct {
    int ok;
    u32 fsize, bsize, frag;        /* fragment / block sizes, frags per block  */
    u32 iblkno, cgoffset, cgmask;  /* cylinder-group inode area + rot layout    */
    u32 ipg, fpg, inopb, nindir;   /* inodes/frags per group, inodes/ptrs per blk*/
} FfsSb;

static FfsSb sb;
/* The image the current call is reading.  There are two in play -- the tape and
   the SCSI disk -- and the parsed superblock is cached, so switching images has
   to invalidate it or the second one is read with the first one's geometry. */
static FILE *ffs_fp;

/* --- raw image access (each op seeks, so sharing the FILE* is safe) --- */
static u32 dread32(u32 byteoff)
{
    u8 b[4];
    if (fseek(ffs_fp, (long)byteoff, SEEK_SET) != 0) return 0;
    if (fread(b, 1, 4, ffs_fp) != 4) return 0;
    return be32(b);
}
static void dread(u32 byteoff, u8 *dst, u32 n)
{
    if (fseek(ffs_fp, (long)byteoff, SEEK_SET) != 0) { memset(dst, 0, n); return; }
    size_t g = fread(dst, 1, n, ffs_fp);
    if (g < n) memset(dst + g, 0, n - g);
}

static int ffs_init(void)
{
    if (sb.ok) return 0;
    if (!ffs_fp) return -1;
    if (dread32(FFS_SBOFF + 0x55c) != FFS_MAGIC) return -1;
    sb.iblkno   = dread32(FFS_SBOFF + 0x10);
    sb.cgoffset = dread32(FFS_SBOFF + 0x18);
    sb.cgmask   = dread32(FFS_SBOFF + 0x1c);
    sb.bsize    = dread32(FFS_SBOFF + 0x30);
    sb.fsize    = dread32(FFS_SBOFF + 0x34);
    sb.frag     = dread32(FFS_SBOFF + 0x38);
    sb.nindir   = dread32(FFS_SBOFF + 0x74);
    sb.inopb    = dread32(FFS_SBOFF + 0x78);
    sb.ipg      = dread32(FFS_SBOFF + 0xb8);
    sb.fpg      = dread32(FFS_SBOFF + 0xbc);
    if (!sb.fsize || !sb.bsize || !sb.ipg || !sb.inopb || !sb.nindir) return -1;
    sb.ok = 1;
    return 0;
}

static u32 blockbyte(u32 fragaddr) { return fragaddr * sb.fsize; }

/* Byte offset of inode `ino`'s 128-byte dinode within its cylinder group. */
static u32 dinode_byte(u32 ino)
{
    u32 cg = ino / sb.ipg;
    u32 cgstart = sb.fpg * cg + sb.cgoffset * (cg & (~sb.cgmask));
    u32 fragaddr = cgstart + sb.iblkno + ((ino % sb.ipg) / sb.inopb) * sb.frag;
    return blockbyte(fragaddr) + (ino % sb.inopb) * FFS_DINODE_SIZE;
}

/* One entry of an indirect block. */
static u32 ind(u32 blkfrag, u32 idx)
{
    if (!blkfrag) return 0;
    return dread32(blockbyte(blkfrag) + idx * 4);
}

/* Frag address of the file's logical block `bn` (direct, single, double). */
static u32 bmap(const u32 *db, const u32 *ib, u32 bn)
{
    u32 nind = sb.nindir;
    if (bn < 12) return db[bn];
    bn -= 12;
    if (bn < nind) return ind(ib[0], bn);
    bn -= nind;
    if (bn < nind * nind) {
        u32 l1 = ind(ib[1], bn / nind);
        return ind(l1, bn % nind);
    }
    return 0;                                   /* triple indirect: unsupported */
}

/* Load inode `ino` into a fresh buffer.  Returns 0 on success and fills mode. */
static int read_ino(u32 ino, u8 **out, u32 *lenp, u32 *mode_out)
{
    u8 din[FFS_DINODE_SIZE];
    dread(dinode_byte(ino), din, FFS_DINODE_SIZE);
    u32 mode = ((u32)din[0] << 8) | din[1];
    u32 size = be32(din + 0x0c);                /* low word of the size quad    */
    u32 db[12], ib[3];
    for (int i = 0; i < 12; i++) db[i] = be32(din + 0x28 + 4 * i);
    for (int i = 0; i < 3;  i++) ib[i] = be32(din + 0x58 + 4 * i);

    u8 *buf = malloc(size ? size : 1);
    if (!buf) return -1;
    u32 nblk = (size + sb.bsize - 1) / sb.bsize;
    for (u32 k = 0; k < nblk; k++) {
        u32 fa = bmap(db, ib, k);
        u32 n = (k + 1) * sb.bsize <= size ? sb.bsize : size - k * sb.bsize;
        if (fa) dread(blockbyte(fa), buf + k * sb.bsize, n);
        else    memset(buf + k * sb.bsize, 0, n);        /* sparse hole */
    }
    *out = buf; *lenp = size; *mode_out = mode;
    return 0;
}

/* Resolve an absolute path within the FFS to an inode number. */
static int namei(const char *path, u32 *ino_out)
{
    u32 ino = FFS_ROOTINO;
    const char *p = path;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        char comp[256]; u32 i = 0;
        while (*p && *p != '/' && i < 255) comp[i++] = *p++;
        comp[i] = 0;

        u8 *dir; u32 dlen, dmode;
        if (read_ino(ino, &dir, &dlen, &dmode)) return -1;
        if ((dmode & 0xF000) != 0x4000) { free(dir); return -1; }   /* not a dir */
        u32 off = 0, found = 0;
        while (off + 8 <= dlen) {
            u32 dino   = be32(dir + off);
            u16 reclen = (u16)((dir[off + 4] << 8) | dir[off + 5]);
            u16 namlen = (u16)((dir[off + 6] << 8) | dir[off + 7]);
            if (!reclen) break;
            if (dino && namlen == i && !memcmp(dir + off + 8, comp, i)) { found = dino; break; }
            off += reclen;
        }
        free(dir);
        if (!found) return -1;
        ino = found;
    }
    *ino_out = ino;
    return 0;
}

/* Public: read a regular file's bytes out of `img`'s FFS.  Caller frees *buf.
   Returns 0 on success, -1 if absent, not a regular file, or unreadable. */
int ffs_read_file(FILE *img, const char *path, u8 **buf, u32 *len)
{
    if (!img) return -1;
    if (img != ffs_fp) { ffs_fp = img; sb.ok = 0; }   /* new image, new geometry */
    if (ffs_init()) return -1;
    u32 ino, mode;
    if (namei(path, &ino)) return -1;
    if (read_ino(ino, buf, len, &mode)) return -1;
    if ((mode & 0xF000) != 0x8000) { free(*buf); return -1; }       /* not regular */
    return 0;
}
