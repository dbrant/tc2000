#!/bin/sh
# Build nx88 as WebAssembly, for the demo page in this directory.
#
#   . ~/emsdk/emsdk_env.sh && ./build.sh          # macOS / Linux
#   PATH="$HOME/emsdk/upstream/emscripten:$PATH" ./build.sh    # Windows, MSYS2
#
# Then serve this directory over HTTP (./serve.py) and open it.  file:// does
# not work: the page starts a Worker and fetches the disk images.
#
# Everything this writes is generated and gitignored:
#   nx88.js  nx88.wasm            the emulator
#   data/vmunix  data/tapeimage.img   staged copies of the tape, fetched by
#                                     the worker at boot time
#
# The source list is emu/build.sh's plus web.c (the browser entry point).
# ffs.c is not in either -- see the header in ffs.c.  No winsock: console.c's
# socket transport compiles out entirely under __EMSCRIPTEN__, replaced by the
# postMessage one; the note at the top of that file explains why the browser
# gets the SOCKET console's line discipline and not the stdio console's.
set -e
cd "$(dirname "$0")"

EMU=../emu
: "${EMCC:=emcc}"

# Where the kernel and the tape's filesystem live.  vmunix turns up in either
# of two places -- inside the extracted tape directory, or on its own beside the
# image once that directory has been thrown away -- so look in both, the same
# way the emulator itself derives the pair (see the candidate list in main.c).
TAPE_IMG=../tapeimage.img
VMUNIX=
for c in ../tapeimage/vmunix ../vmunix; do
    if [ -f "$c" ]; then VMUNIX=$c; break; fi
done

SRCS="globals.c memory.c devices.c mmu.c cpu.c aout.c kmsg.c \
      usermode.c console.c proc.c sysmode.c main.c web.c"

debug=0
[ "$1" = "--debug" ] && debug=1

# -O2, not -O3: the emulator is one big interpreter switch and -O3 buys it
# nothing measurable here while costing a third again in .wasm bytes to
# download.
CFLAGS="-O2 -Wall"
LINKFLAGS="
    -sASYNCIFY=1
    -sASYNCIFY_STACK_SIZE=65536
    -sALLOW_MEMORY_GROWTH=1
    -sINITIAL_MEMORY=67108864
    -sMAXIMUM_MEMORY=536870912
    -sSTACK_SIZE=1048576
    -sFORCE_FILESYSTEM=1
    -sMODULARIZE=1
    -sEXPORT_NAME=createNx88
    -sENVIRONMENT=web,worker,node
    -sINVOKE_RUN=0
    -sEXIT_RUNTIME=0
    -sEXPORTED_RUNTIME_METHODS=ccall,FS
    -sEXPORTED_FUNCTIONS=_main,_nx_web_run,_nx_web_push,_nx_web_eof,_malloc,_free
"
# ASYNCIFY notes, because the numbers here are not arbitrary:
#   * ASYNCIFY_STACK_SIZE must hold the C stack that emscripten_sleep() unwinds.
#     The deepest one is main -> run_sys -> console_syscall -> con_read_line ->
#     con_sock_read -> con_cook_line -> con_in_byte -> con_recv_raw -> web_getc,
#     which fits in far less than 64K; the default 4K is uncomfortably close.
#   * The emulator has NO function pointers (checked: nothing in emu/ takes an
#     address of a function), so emcc's call-graph analysis is exact and only
#     the handful of functions on that chain get instrumented.  In particular
#     step(), the interpreter's hot loop, does not -- the syscall dispatch lives
#     in run_sys, one frame above it.
#   * STACK_SIZE is the ordinary C stack.  console_syscall alone puts a 4K
#     buffer on it and the default is only 64K.
# `node' is in ENVIRONMENT purely so the build can be tested without a browser:
# ./selftest.js drives a whole boot-and-shell session head-less.
if [ "$debug" = 1 ]; then
    CFLAGS="-O1 -Wall -g"
    LINKFLAGS="$LINKFLAGS -sASSERTIONS=2 -sSAFE_HEAP=0 -sSTACK_OVERFLOW_CHECK=2"
fi

echo "compiling nx88.wasm ..."
# shellcheck disable=SC2086
(cd "$EMU" && exec $EMCC $CFLAGS $SRCS -o ../web/nx88.js $LINKFLAGS)

# Stage the two host files the emulator opens: the kernel it loads as an a.out,
# and the tape's UFS it reads as the root filesystem.  (disk.img, the SCSI
# install target, is deliberately NOT here -- it is 256 MB and nothing in the
# --shell demo touches it.  The emulator runs fine without one.)
mkdir -p data
if [ -z "$VMUNIX" ]; then
    echo "MISSING: no vmunix at ../tapeimage/vmunix or ../vmunix" >&2
    exit 1
fi
for f in "$VMUNIX:data/vmunix" "$TAPE_IMG:data/tapeimage.img"; do
    src=${f%:*}; dst=${f#*:}
    if [ ! -f "$src" ]; then
        echo "MISSING: $src -- extract the tape first (see emu/BOOT.md)" >&2
        exit 1
    fi
    if [ ! -f "$dst" ] || [ "$src" -nt "$dst" ]; then
        echo "staging $src -> $dst"
        cp "$src" "$dst"
    fi
done

echo
echo "built:"
ls -l nx88.js nx88.wasm data/vmunix data/tapeimage.img | awk '{printf "  %10s  %s\n", $5, $9}'
echo
echo "now run ./serve.py and open http://localhost:8000/"
