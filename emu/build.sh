#!/bin/sh
# Build nx88 from the split sources.  MSYS2 UCRT64 gcc:
#   export PATH="/c/msys64/ucrt64/bin:$PATH" && ./build.sh
# The running nx88.exe holds a Windows lock, so kill it first.
set -e
taskkill //F //IM nx88.exe >/dev/null 2>&1 || true
gcc -O2 -Wall -o nx88.exe \
    globals.c memory.c devices.c mmu.c cpu.c aout.c kmsg.c \
    usermode.c console.c proc.c sysmode.c main.c \
    -lws2_32
echo "built nx88.exe"
