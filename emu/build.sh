#!/bin/sh
# Build nx88 from the split sources.
#
#   ./build.sh              from emu/
#   ./emu/build.sh          from the repo root -- same thing; see the cd below
#
# Windows (MSYS2 UCRT64): export PATH="/c/msys64/ucrt64/bin:$PATH" first.  The
# running nx88.exe holds a file lock there, so it is killed before linking, and
# the telnet console needs winsock.  Neither applies on macOS/Linux.
#
set -e
# The source list below is bare filenames, so the build has to run from here.
# Anchoring to the script's own directory -- which web/build.sh and serve.py
# already do -- is what lets it be invoked by path from anywhere.
cd "$(dirname "$0")"

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        BIN=nx88.exe
        taskkill //F //IM nx88.exe >/dev/null 2>&1 || true
        LIBS="-lws2_32"
        ;;
    *)
        BIN=nx88
        LIBS=""
        ;;
esac

${CC:-cc} -O2 -Wall -o "$BIN" \
    globals.c memory.c devices.c mmu.c cpu.c aout.c kmsg.c ffs.c \
    usermode.c console.c proc.c sysmode.c main.c \
    $LIBS
echo "built $BIN"
