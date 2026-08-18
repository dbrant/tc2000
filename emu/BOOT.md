# Booting nX on the TC2000 emulator — the boot arc

This documents how `nx88.c` takes the recovered **BBN TC2000** installation tape from
a raw image to a **fully booted nX kernel** sitting in its swapper idle loop with the
root filesystem mounted. It is a map of the journey and the machine, not a line-by-line
reference.

## The machine

- **CPU:** Motorola **88100** + **88200 CMMU**, big-endian. Kernel virtual base `0xC0000000`,
  vmunix entry `0xC0016C08`.
- **OS:** nX = a **Mach VM layer** (`pmap_*`, `vm_*`) over **4.3BSD** + Sun NFS. FFS root,
  8192-byte blocks / 1024-byte fragments.
- **Interconnect:** the BBN **Butterfly switch** with memory interleaving; per-node control via
  the **TCS** (Test & Control System), a shared-memory mailbox.
- **Boot disk:** SCSI via an **Interphase 4210** Host Adapter (`sha.o`).

Physical addresses carry a node field: `[mode:31-30][node:29-23][offset:22-0]` — mode 3 =
node-local, mode 2 = interleaved/global. The kernel is linked at `0xC0010000`, already a
valid node-local physical address, so the boot loader's map is essentially identity/direct.

## Running it

```sh
export PATH="/c/msys64/ucrt64/bin:$PATH"
./build.sh                                         # or: make
./nx88 sys <path-to>/tapeimage.img             # the image IS the machine
```

The emulator is split by theme across a dozen small `.c` files plus a shared
`nx88.h`; see the header's top comment for the module map. All machine state
and configuration flags live in `globals.c`; each other file holds one
subsystem's behaviour.

### One file: `sys tapeimage.img`

The tape image is a filesystem, and `/vmunix` is a file inside it — so the image
alone is a complete machine:

```sh
./nx88 sys <path-to>/tapeimage.img --shell --kmsg
```

The emulator reads the kernel out of the image through `ffs.c` and mounts that
same image as root. `--kernel=PATH` selects a different kernel from within it;
`--tape=PATH` mounts a different filesystem as root than the one booted from.

### A note on MSYS2 and guest paths

Flags whose value is an absolute *guest* path — `--uprog=/bin/date`,
`--kernel=/vmunix` — are rewritten by MSYS2's argument conversion into Windows
paths before the emulator ever sees them, which silently changes what runs.
Under MSYS2 bash, prefix the command with `MSYS2_ARG_CONV_EXCL='*'` (and then
give host paths in Windows form, since those genuinely do need converting).
PowerShell and cmd are unaffected.

Defaults: the **real-memory model** (the default) with translation and EEPROM
signature `'A'`.  `--identity` selects the superseded identity+fallback path.
Useful flags: `--scsi` (configure the SCSI disk / enable the sd0 path),
`--scsitrace`, `--clock`, `--pchist` (dump the last 64K PCs at halt — the tool
that cracked several blockers), `--watch=PC`, `--nodes=N`, and the two image
paths `--tape=PATH` (mount a different root than the booted image) and
`--disk=PATH` (SCSI sd0 install target; default `disk.img`).

### What reaches the terminal — `--debug`

Two different voices used to share stdout, and the louder one was not the
interesting one. What the **machine** says — the kernel's log (`--kmsg`) and the
guest process's own output — is what someone running it came for. What the
**emulator** says about the machine — the load map, the synthetic boot tables,
the proc experiment's register dumps, `[kfall]`, `[halt]`, `[PANIC]`, the
closing instruction count — is debugging, and there is enough of it to bury the
kernel's log completely.

All of the latter now goes through `dbg()` (see `nx88.h`) and needs `--debug`:

```sh
./nx88 sys <path-to>/tapeimage.img --shell           # the machine only
./nx88 sys <path-to>/tapeimage.img --shell --kmsg    # + the kernel's log
./nx88 sys <path-to>/tapeimage.img --shell --debug   # + everything else
```

Every *other* diagnostic flag (`--trace-traps`, `--wmem=`, `--pwatch=`,
`--profile`, `--pchist`, `--vmprobe`, `--ctxtrace`, `--scsitrace`, `--watch=`,
`--uprog=`, `--handload=`, `-v`, …) switches `--debug` on for itself, so no
command line here prints less than it used to; what changed is the bare
`sys <image>` and `--shell` runs. `--quiet` switches it back off, and options are
read left to right, so the last one wins. Genuine failures — a tape image that
will not open, a bad a.out — are not commentary and stay on stderr regardless.

### The kernel's own boot messages — `--kmsg`

`--kmsg` prints everything the kernel says, formatted by the kernel itself. It
hooks `putchar` in `subr_prf.o` (`0xC005B218`), the funnel every `printf`
character passes through, rather than reconstructing messages from format
strings. Combines with anything:

```sh
./nx88 sys <path-to>/tapeimage.img --shell --kmsg
```

```
[nx] Configuring the processors ...
[nx] nX Operating System (TC2000) #191: Tue Nov 28 18:33:02 1989
[nx] Physical memory = 1024.00 megabytes.
[nx] Buffer allocation across Bay.Midplane.Node[0 - 7]:
[nx] 7.0.*:   0x0   0x28  0x29  0x29  0x29  0x29  0x29  0x29
[nx] BBN TC2000
[nx] Probing for VMEbus
[nx] mb0 is iobus 0 on processor node 0.7.7
[nx] xyprobe: error on xy controller at fc000e40, csr = 0
[nx] Cluster startup: 64 node system ... 1 node public cluster
[nx] Configuring SCSI devices
[nx] WARNING: TOY clock not found, setting time from file system
[nx] Root fstype 4.3
[nx] network interface lo, unit 0, on iobus -3 is "lo0"
```

Every message goes through `putchar` **twice** — once with flags=1 for the
console and again with flags=4 for msgbuf/syslog — while `log()` messages with
a priority (the `<6>...` ones) take only the second path. So the two streams
are assembled into lines separately and a syslog line is printed only when it
isn't the one the console just showed. That yields each message exactly once,
console and syslog alike.

**Colour.** The log is coloured on a real terminal: the machine's identity lines
bold bright white, anything reporting a failure amber (the tape's hardware probes
find plenty), and the kernel's own `log()` priority lines — `<6>` allocator sizes
and internal bookkeeping — dimmed. The rules live in one function, `kmsg_sgr` in
`kmsg.c`, and are deliberately the same three the browser page applies, so a boot
log reads alike in either place.

It is on only when something can render it: `--color` forces it, `--no-color` and
`NO_COLOR=1` turn it off, and the default is on for a terminal and **off when
redirected**, since escape codes in a saved boot log are worse than no colour.

The browser build uses this same path rather than styling the log in JavaScript.
xterm.js renders the ANSI natively, `kmsg_line` already holds each complete line
where the page would have had to reassemble one out of arbitrary byte chunks, and
one set of rules cannot drift out of step with a second. The page opts in with
`--color` in its argument string, because it does not look like a terminal to
`isatty()` — it installs its own stdout handler, which Emscripten backs with a
character device rather than a tty.

Only the `[nx]` log is ever coloured; the guest's own output is passed through
untouched, which is why the shell banner mentions TC2000 and stays plain.

There is a **third** copy of every message, and it is behind `--debug`. The kernel also queues
every console message down the TCS mailbox ring to the front-end processor,
which on a 512-node machine multiplexes all of them and so tags each line with
its origin: `0.0.0   0: Probing for VMEbus`. It only appears with `--clock`,
because the ring is drained from `_hps_poll` — software interrupt source 14 —
and nothing dispatches software interrupts until the clock runs. Being the same
text a second time, it would double the whole boot log under `--kmsg`, so it
lives with the emulator's other hardware-level commentary. Note that the *drain*
is unconditional whatever the flags: `_outputwakeup` spins until the consumer
index catches the producer.

### An interactive shell

```sh
./nx88 sys <path-to>/tapeimage.img --shell
```

boots the kernel and hands your terminal to `/bin/sh` off the 1989 tape:

```
=== nX on the TC2000 -- /bin/sh from the 1989 install tape ===
The root filesystem is the tape's own UFS.  ^D or `exit' quits.

# pwd
/
# cat /etc/passwd
root::0:10::/:/bin/sh
# ls -l /etc/passwd
-rw-rw-r--  1 root                 22 Nov 28 19:06 /etc/passwd
# date
Tue Nov 28 19:07:37 EST 1989
```

`cd`, `ls`, `cat`, `grep`, `df`, pipelines, `$?` and shell variables all work.
The filesystem is genuinely the tape's UFS, read block by block through the
kernel's own code — so only what shipped on the installer is there (`wc` and
`who`, for instance, are not). It is read-only in effect: writes go into the
buffer cache and no further, because no disk is modelled yet.

### A detachable console — `--console-port`

By default the shell shares the emulator's stdin/stdout with the kernel log.
`--console-port=N` splits them: the kernel boot log keeps flowing to stdout, and
the interactive session is served on a loopback TCP socket at `127.0.0.1:N` for a
VT100/telnet client to attach to.

```sh
./nx88 sys <path-to>/tapeimage.img --shell --console-port=2323 --kmsg
# ...boot log on stdout... then in another window:
telnet 127.0.0.1 2323        # or: nc 127.0.0.1 2323, or PuTTY (Raw/Telnet)
```

The emulator boots (log to stdout), then blocks at the shell banner until a
client connects. The socket is a serial-console stand-in and does the terminal
cooking the host tty used to do for us: LF→CRLF on output, CR/CRLF/CR-NUL→LF on
input, server-side echo with backspace line-editing (a whole cooked line is
buffered and drained across the many one-byte reads `/bin/sh` issues), and `^D`
as end-of-file. A little telnet IAC negotiation (`WILL ECHO`, `WILL/DO SGA`)
puts a real telnet client into character mode; raw-TCP clients ignore it and
behave the same. Closing the client sends EOF, so the shell exits and the
emulator with it.

A clean default run ends with:

```
[boot-complete] kernel reached the swapper sched() idle loop @55243479 -- main() done, root mounted.
```

### Populating and running from the SCSI disk

**The disk can be any size.** `--disk=PATH` takes an image of whatever size you
make it — `truncate`/`fsutil` a new one and attach it. nX's `newfs` refuses a
disk whose label it cannot read ("Label style not understood yet"), so the
emulator writes a synthetic Sun disklabel into block 0, and derives the
geometry from the file: heads and sectors/track are a conventional 16 × 63, and
the size lands in the cylinder count. That is recomputed on **every** attach and
rewritten when it has gone stale, which is what lets an image be resized —
otherwise a label written once describes whatever the file used to be, and
growing a 64 MB image to 128 MB leaves half of it unreachable with nothing to
say so. A label that already matches is left alone, byte for byte.

The guest sees it: `newfs /dev/rsd0b` on a 32 MB image reports `65520 sectors in
65 cylinders`, and on a 96 MB one `196560 sectors in 195 cylinders`. The ceiling
is 2 GB, because the whole sd0 path seeks with `long` offsets.

Once `newfs` + `mount` gave a real FFS on `disk.img`, three pieces let the guest
fill it and run programs out of it:

- **`--hostfile=PATH`** exposes a host file to the guest at the synthetic path
  `/hosttar`, read-only, serviced straight from the host (parallel to the raw
  `sd0` path).  So the guest can `mount /dev/sd0b /mnt; cd /mnt; tar xpf /hosttar`
  to unpack an archive that lives on no guest filesystem.  The view is zero-padded
  past EOF to a full tar record, because archives in hand may be truncated (no
  terminator) and the 1989 BSD `tar` is stricter than GNU tar.
- **`ffs.c`** is a read-only 4.3BSD FFS reader over `disk.img` (superblock → name
  lookup → inode → direct/indirect blocks; big-endian, offsets pinned against a
  guest-populated image).
- **`--diskmount=DIR`** tells the emulator where `disk.img` is mounted, so the
  `execve` shortcut, when a binary isn't on the host tape mirror, resolves the
  path within `disk.img`'s FFS and loads the a.out from there.

The a.out loader handles **two layouts**: nX/m88k (`mid` 0 — 8192-byte header
page, text at file offset 8192, load VA 0) and standard ZMAGIC (`mid` != 0 —
text at file offset 0, load VA 0x2000).  Verified end to end: an m88k binary
copied onto `disk.img` (`cp /bin/cat /mnt/catm88`) loads through the FFS reader
and runs (`echo hi | /mnt/catm88`).

Note on the `usr.tar` in this collection: it is a **Sun-3 (MC68020, a.out
`mid` 2)** `/usr` — `mnt/bin/sun3cvt`, a whole `mnt/include/sun3/` tree, m68k
code (`4eb9` JSR abs-long).  Its files copy onto `disk.img` and are readable,
but they are the wrong architecture to execute on the m88k TC2000 emulator.

## The boot arc, in order

Each of these was a distinct blocker that had to be understood and cleared.

1. **Synthetic boot loader.** No boot loader survives (it's in ROM / `/stand/secboot`), so
   `boot_build_tables()` synthesizes the initial segment/page tables the kernel expects,
   mapping its virtual addresses to where it is loaded. CMMU probes: 0-hit → all-hit.
2. **Early bring-up stubs.** EEPROM signature (`--sig=A`), CMMU ID registers, the TCS
   shared-memory mailbox (`tcs_poke`), the free-running timer, the interleaver/CMRAM window,
   and the b2vme master-mapper free list — small register/memory models, enough to boot.
3. **Real-memory model (realmm).** The kernel runs under genuine translation from early boot
   so its own page-table writes land coherently; kernel space is a global direct map
   (VA−`0xC0000000`), device space identified, per-node "vv" windows aliased onto the single
   node's `.data`. This got past the VM/pmap coherence ceiling the identity model hit.
4. **MC88100 interrupt delivery + clock.** Exception entry (`deliver_exception`), a corrected
   `rte` (resume at first-valid of SXIP/SNIP/SFIP), `ff0/ff1`, and a periodic hardclock.
5. **CMRAM text-corruption fix.** The interleaver pool walk was zeroing kernel text through the
   CMRAM data port; gave CMRAM its own backing store. (This had masqueraded as a VM panic,
   `vnode_mapping_decr`.)
6. **Disk I/O completion — the big reframe.** The panic long blamed on "the scheduler"
   (`sleep_and_unlock: sleeping idle proc`) was actually a **buffer-cache completion gap**:
   `biowait` never saw `B_DONE` because our synchronous SCSI completion never ran `biodone`.
   Fixed by marking the buffer done and releasing its lock at the wait.
7. **Real root reads.** The root is the **tape's UFS filesystem** (`tapeimage.img`, superblock
   at byte `0x2000`); `disk.img` stays blank as the install target. The biowait now reads
   `tapeimage.img[b_blkno*512 : +b_bcount]` into the buffer (`struct buf`: flags `+0x00`,
   bcount `+0x14`, data addr `+0x3c`, blkno `+0x40`). The UFS mount reads and validates the
   superblock and proceeds.
8. **m88100 FPU.** The scheduler's load-average update (`fadd/fsub/fmul` on IEEE doubles) hit
   an unimplemented opcode; implemented SFU1 (opcode `0x21`) with single/double register-pairs.
9. **Node count / RTC sync.** The node-config device read all 512 slots as "present" → the
   kernel believed it had 64 nodes and timed out synchronizing 63 phantom RTCs
   (`panic: synchrtc`). Cross-node RTC sync is meaningless on one emulated node, so it's
   skipped; `--nodes=N` can seed a true node count (single-node reveals a later userland gap,
   see below).
10. **Boot complete.** `main()` finishes: root mounted, `idle_cpu` and daemon kernel threads
    created, and proc0 falls into `for(;;) sleep(&proc0)` — the swapper's `sched()` idle loop.

## The emulator, in brief

- **CPU core:** full 88100 decode incl. delay slots, triadic form, control registers,
  bit-field ops, `ff0/ff1`, the FPU, and exception/`rte` handling.
- **MMU:** 88200 two-level translation with a per-side TLB; realmm gives kernel space a global
  direct map so translation stays coherent across address-space switches.
- **Devices (stub-level):** TCS mailbox, timer, interleaver/CMRAM, b2vme master mapper, and the
  SHA SCSI adapter — modeled just far enough to boot.
- **Synchronous I/O model:** rather than drive the interrupt-driven SCSI/biodone path (which is
  gated by process context that doesn't exist yet), disk reads are satisfied synchronously from
  the root image at the buffer-cache wait. This is the pragmatic spine that got the boot to
  root-mount.

## Where userland begins (the next phase)

The context-switch machinery **works** — `load_context` (`_local_phys_pte_page`, `c0017498`)
runs 64× during boot to create the per-CPU idle threads. The gap is upstream: **the executed
`main()` never creates init.** Its whole sequence spawns only kernel threads (`idle_cpu`×64,
`shaconfig`, `thread-wirer`) and then sleeps as the swapper — there is no `newproc`/fork/icode
step, and `_fork`/`_procdup` are never called. `/etc/init` exists in the binary but is never
referenced along this path.

So reaching the installer is a new project phase, needing: (1) nX's actual first-process launch
mechanism (a Mach bootstrap task, or the master processor's slave-start path — not yet located);
(2) **user-mode-under-kernel execution** — syscall traps from user code, page-fault demand
paging of a user address space, copyin/copyout — which the emulator has not yet exercised;
(3) `execve("/etc/init")` off the mounted tape UFS; and (4) init running the install script
(many syscalls + real writes to `disk.img`).

The kernel-boot milestone is banked and solid. Crossing into userland starts here.
