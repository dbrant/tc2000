#!/bin/sh
# Build a bootable nX root disk from the install tape.
#
#   ./tools/mkrootdisk.sh [image] [size-MB] [extra.tar ...]
#
# e.g.  ./tools/mkrootdisk.sh newroot.img 24 games.tar
#
# The result boots on its own -- `./emu/nx88 sys newroot.img --shell' -- with
# no tape and no separate kernel, because the image carries /vmunix like the tape
# does and the emulator reads it out of the filesystem it is about to mount.
#
# Three things about this are worth knowing before changing it:
#
#  * /dev CANNOT be copied.  `cp -r /dev' would READ every node -- pulling the
#    whole raw disk through /dev/rsd0b -- and the 1989 tar does not carry device
#    files either.  The nodes are RECREATED with mknod, from the major/minor
#    numbers `ls -l /dev' prints.  nX's mknod takes an optional iobus argument
#    and `ls' shows it as -1/-3, but it rejects those as arguments; omitted, it
#    defaults to -1, which /dev/null and the rest work fine with.
#
#  * The session MUST end with umount.  The emulator services block I/O
#    synchronously at the point the kernel waits for it, so writes nothing waits
#    on are simply lost when the machine stops.  umount waits.  Skip it and the
#    image gets a filesystem whose bitmaps disagree with its directory tree,
#    which surfaces in a LATER run as `panic: ialloc: dup alloc'.
#
#  * A newly booted root is READ-ONLY in effect, for the same reason.  To change
#    a disk, attach it as --disk from a tape boot and umount -- which is exactly
#    what this script does.
set -e
cd "$(dirname "$0")/.."

IMG=${1:-newroot.img}
MB=${2:-24}
shift 2 2>/dev/null || true

NX=./emu/nx88                      # ./emu/nx88.exe on Windows; see below
TAPE=tapeimage.img
TMP=${TMPDIR:-/tmp}/mkrootdisk.$$
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP"

# The Windows build carries a .exe; -x on the bare name does not see it, even
# though executing that name would work.
[ -x "$NX" ] || [ -x "$NX.exe" ] || {
    echo "no $NX -- run emu/build.sh first" >&2; exit 1; }
[ -x "$NX" ] || NX="$NX.exe"
[ -f "$TAPE" ] || { echo "no $TAPE" >&2; exit 1; }

echo "creating $IMG (${MB} MB)"
dd if=/dev/zero of="$IMG" bs=1048576 count="$MB" 2>/dev/null

# --- the device table, straight from the tape's own /dev -------------------
echo "reading the tape's device table"
printf 'ls -l /dev\n\004' | "$NX" sys "$TAPE" --shell 2>/dev/null > "$TMP/devs"
awk '{ sub(/^[# ]+/, "") }
     /^[cb]/ {
        typ = substr($0, 1, 1)
        maj = $4; sub(",", "", maj)
        mnr = $5; sub(",", "", mnr)
        name = $NF
        if (maj ~ /^-?[0-9]+$/ && mnr ~ /^-?[0-9]+$/)
            printf "/etc/mknod /mnt/dev/%s %s %s %s\n", name, typ, maj, mnr
     }' "$TMP/devs" > "$TMP/mknods"
echo "  $(grep -c . "$TMP/mknods") device nodes"

# --- newfs, copy the root, recreate /dev, flush ----------------------------
{
    echo '/etc/newfs /dev/rsd0b'
    echo '/etc/mount /dev/sd0b /mnt'
    for f in vmunix .profile .login bootconf; do echo "cp /$f /mnt/$f"; done
    for d in bin etc usr DIST; do echo "cp -r /$d /mnt/$d"; done
    echo 'mkdir /mnt/dev'
    echo 'mkdir /mnt/tmp'
    echo 'chmod 777 /mnt/tmp'
    cat "$TMP/mknods"
    echo 'chmod 666 /mnt/dev/null'
    echo 'chmod 622 /mnt/dev/console'
    echo 'sync'
    echo '/etc/umount /dev/sd0b'      # the one that must not be skipped
    echo 'sync'
} > "$TMP/build.guest"

echo "building the filesystem"
"$NX" sys "$TAPE" --shell --scsi --disk="$IMG" < "$TMP/build.guest" > "$TMP/log" 2>&1 || true
grep -iE 'WARNING|panic' "$TMP/log" && { echo "build did not flush cleanly" >&2; exit 1; }
grep -E 'sectors in .* cylinders' "$TMP/log" | sed 's/^/  /'

# --- optional extra archives, each unpacked at the root of the new disk ----
for tarball in "$@"; do
    [ -f "$tarball" ] || { echo "no $tarball -- skipped" >&2; continue; }
    echo "unpacking $tarball"
    printf '/etc/mount /dev/sd0b /mnt\ncd /mnt\ntar xpf /hosttar\nsync\n/etc/umount /dev/sd0b\n\004' \
        | "$NX" sys "$TAPE" --shell --scsi --disk="$IMG" --hostfile="$tarball" \
          > "$TMP/log" 2>&1 || true
    grep -iE 'WARNING|panic' "$TMP/log" && { echo "unpack did not flush cleanly" >&2; exit 1; }
done

echo
echo "done -- boot it with:"
echo "    $NX sys $IMG --shell --kmsg"
