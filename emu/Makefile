# nx88 -- MC88100 emulator for BBN nX (TC2000).
#
# Build with MSYS2 UCRT64 gcc:
#   export PATH="/c/msys64/ucrt64/bin:$PATH" && make
#
# On Windows the running nx88.exe holds a lock; `make` kills it first.

CC      := gcc
CFLAGS  := -O2 -Wall
LDFLAGS :=
LDLIBS  :=

SRCS := globals.c memory.c devices.c mmu.c cpu.c aout.c kmsg.c ffs.c \
        usermode.c console.c proc.c sysmode.c main.c
OBJS := $(SRCS:.c=.o)
HDR  := nx88.h
BIN  := nx88.exe

.PHONY: all clean

all: $(BIN)

$(BIN): $(OBJS)
	-@taskkill //F //IM nx88.exe >/dev/null 2>&1 || true
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS) $(LDLIBS)

%.o: %.c $(HDR)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(BIN)
