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
./nx88.exe sys <path-to>/tapeimage/vmunix          # root image auto-derived: <tapedir>.img
```

The emulator is split by theme across a dozen small `.c` files plus a shared
`nx88.h`; see the header's top comment for the module map. All machine state
and configuration flags live in `globals.c`; each other file holds one
subsystem's behaviour.

Defaults: the **real-memory model** (the default) with translation and EEPROM
signature `'A'`.  `--identity` selects the superseded identity+fallback path.
Useful flags: `--scsi` (configure the SCSI disk / enable the sd0 path),
`--scsitrace`, `--clock`, `--pchist` (dump the last 64K PCs at halt — the tool
that cracked several blockers), `--watch=PC`, `--root=PATH`, `--nodes=N`.

### The kernel's own boot messages — `--kmsg`

`--kmsg` prints everything the kernel says, formatted by the kernel itself. It
hooks `putchar` in `subr_prf.o` (`0xC005B218`), the funnel every `printf`
character passes through, rather than reconstructing messages from format
strings. Combines with anything:

```sh
./nx88.exe sys <path-to>/tapeimage/vmunix --shell --kmsg
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

### An interactive shell

```sh
./nx88.exe sys <path-to>/tapeimage/vmunix --shell
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

A clean default run ends with:

```
[boot-complete] kernel reached the swapper sched() idle loop @55243479 -- main() done, root mounted.
```

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
