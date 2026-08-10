#!/bin/sh
# Build nx88 from the split sources.
#
#   ./build.sh
#
# Windows (MSYS2 UCRT64): export PATH="/c/msys64/ucrt64/bin:$PATH" first.  The
# running nx88.exe holds a file lock there, so it is killed before linking, and
# the telnet console needs winsock.  Neither applies on macOS/Linux.
#
# ffs.c is deliberately NOT in the link: its only consumer was --diskmount in
# the synthetic process model, which is gone.  The file stays because it is
# still the way to read disk.img from the host -- see the header in ffs.c.
#
# The binary keeps the name nx88.exe on every platform so the commands in
# BOOT.md (and everyone's shell history) work unchanged on both machines.
set -e

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        taskkill //F //IM nx88.exe >/dev/null 2>&1 || true
        LIBS="-lws2_32"
        ;;
    *)
        LIBS=""
        ;;
esac

${CC:-cc} -O2 -Wall -o nx88.exe \
    globals.c memory.c devices.c mmu.c cpu.c aout.c kmsg.c \
    usermode.c console.c proc.c sysmode.c main.c \
    $LIBS
echo "built nx88.exe"
