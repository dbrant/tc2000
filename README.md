# TC2000

An emulator for the **BBN TC2000**, a Motorola 88100 massively parallel
supercomputer from 1989 — complete enough to boot **nX**, BBN's Unix, from a
recovered installation tape and drop you at a shell prompt.

```
[nx] nX Operating System (TC2000) #191: Tue Nov 28 18:33:02 1989
[nx] Physical memory = 16.00 megabytes.
[nx] BBN TC2000
[nx] Probing for VMEbus
[nx] Cluster startup: 1 node system ... 1 node public cluster
[nx] Configuring SCSI devices
[nx] WARNING: TOY clock not found, setting time from file system
[nx] Root fstype 4.3

# date
Tue Nov 28 19:07:37 EST 1989
```

No boot ROM survives, and there is no source for the kernel and no register map
for the hardware — the machine model was inferred from what the kernel *does*
when you run it, one blocker at a time. The instruction set was worked out the
same way, from the kernel binary itself.

It also [runs in a browser](web/).

---

## Quick start

You need exactly one file that is data, not code, and is not in this repo: the
dumped tape image, `tapeimage.img`.

```sh
./emu/build.sh                                  # any C compiler; see below
./emu/nx88 sys tapeimage.img --shell --kmsg
```

That is the whole machine in one file. The image is a 4.3 BSD filesystem and the
kernel is a file inside it, so the emulator reads `/vmunix` out of the image and
then mounts that same image as root. `--kernel=PATH` boots a different one from
within the image.

`--shell` boots the kernel and hands your terminal to `/bin/sh` off the boot
image; `--kmsg` shows the kernel's own console log. The kernel reaches a mounted
root in about 3.7 million emulated instructions, which at ~25 million
instructions/second is a fraction of a second.

```
# ls /bin
STTY            cp              ed              mkdir           sh
[               csh             expr            mt              stty
awk             date            grep            mv              sync
cat             dd              ln              pwd             tar
chmod           df              ls              rm              test
clear           echo            make            runincluster
# cat /etc/passwd
root::0:10::/:/bin/sh
```

`cd`, `ls`, `cat`, `grep`, `df`, pipelines, `$?` and shell variables all work.
The filesystem is genuinely the tape's UFS, read block by block through the
kernel's own code, so only what shipped on the installer is there. **Ctrl-D**
halts the machine (`exit` will not — this shell runs it and carries on).

On Windows, build under MSYS2:
`export PATH="/c/msys64/ucrt64/bin:$PATH"`. The binary is `nx88.exe` there and
plain `nx88` elsewhere, but every command in these docs is written `./nx88` and
works on all three — MSYS, Cygwin, cmd and PowerShell all append the extension
when resolving a program name.

---

## The machine

BBN — Bolt, Beranek and Newman, the company that built the ARPANET's routers —
was commissioned by DARPA in 1978 to design a parallel computer. What came out
of it was the **Butterfly**, named for the shape of its interconnect: the wiring
topology of its multistage switching network resembles the FFT butterfly
diagram.

The family ran through two decades of Motorola silicon:

| | CPU | Max CPUs | |
|---|---|---|---|
| Butterfly | MC68000 | | early 1980s |
| Butterfly GP1000 | MC68020 | 256 | "GP" = General Purpose |
| **Butterfly TC2000** | **MC88100** | **512** | "TC" = Time Critical, July 1989 |

The TC2000 was the last of them, aimed at the real-time market, and by 1991 it
was the only machine BBN was still actively selling.

### Architecture

Each node carried a 20 MHz **Motorola 88100** RISC processor — Motorola's
short-lived 88000 architecture, big-endian, with explicit branch delay slots —
paired with **88200 CMMUs** for translation and caching, plus its own local
memory. Up to 512 of these plugged into the **butterfly switch**, built from
8×8 crossbar modules.

The result was shared memory, but not uniform shared memory. Every processor
could address every other processor's memory through the switch, at roughly
**15× the latency** of its own. That single ratio is the whole design problem of
the machine, and it shows up everywhere in nX — in the `fork_and_bind` call that
pins a child process to a chosen node, in the "cluster" abstraction that carves
the machine into groups, and in the per-node buffer-cache accounting the kernel
prints at boot.

Physical addresses carry a node number in them:

```
[mode:31-30][node:29-23][offset:22-0]      mode 3 = node-local, 2 = interleaved
```

Nodes are named by physical position — `Bay.Midplane.Node` — which is why the
boot log talks about `processor node 0.7.7`. A separate **TCS** (Test & Control
System), reached through a shared-memory mailbox, handled per-node control and
diagnostics. I/O hung off VMEbus; the boot disk is SCSI behind an Interphase
4210 host adapter.

### nX

nX is **4.3 BSD userland and syscalls over a Mach VM layer** (`vm_map`, `pmap_*`,
`vm_fault`, copy-on-write), with Sun NFS and an FFS root. It is recognisably
late-80s BSD: terminals are `sgtty`, not termios; binaries are a.out; `/bin/sh`
is the Bourne shell.

The kernel in this repository is build **#191, dated 28 November 1989** — about
four months after the TC2000 was announced.

---

## Using the emulator

Two modes:

```sh
nx88 sys  <image>  [flags]      # boot a filesystem image
nx88 user <binary> [args]       # run one m88k a.out with no kernel at all
```

### What reaches your terminal

By default the emulator says **nothing**. What you see is what the machine said:

```sh
./nx88 sys tapeimage.img --shell           # the guest only
./nx88 sys tapeimage.img --shell --kmsg    # + the kernel's log
./nx88 sys tapeimage.img --shell --debug   # + the emulator's commentary
```

`--debug` unlocks the load map, the synthetic boot tables, the process
experiment's register dumps, `[kfall]`/`[halt]`/`[PANIC]` and the closing
instruction count. Every *other* diagnostic flag turns it on for itself, so no
command line prints less than it did before the gate existed; `--quiet` turns it
back off, and the last flag on the line wins.

### Running programs

| flag | |
|---|---|
| `--shell` | boot, then run the guest's `/bin/sh` as a real nX process |
| `--uprog=PATH` | run any guest binary the same way — the kernel's own `execve`, `fork`, copy-on-write, page faults and scheduler. `--uarg=`/`--uenv=` supply argv/envp |
| `--handload=HOSTPATH` | run a host a.out that exists on no guest filesystem; the emulator loads the image itself. Single programs only |
| `--init` | also let init (pid 1) run |

`--uprog` is the real path: the emulator does not simulate processes, it lets
the kernel create them.

### Machine configuration

| flag | |
|---|---|
| `--tape=PATH` | mount a different filesystem as root than the one booted from |
| `--kernel=PATH` | boot a different kernel from inside the image (default `/vmunix`) |
| `--disk=PATH` | SCSI `sd0` install target (default `disk.img`) |
| `--hostfile=PATH` | expose a host file to the guest at `/hosttar`, read-only — so the guest can `tar xpf /hosttar` an archive that lives on no guest filesystem |
| `--nodes=N` | how many nodes the machine has |
| `--clock` | a real microsecond timer and periodic hardclock |
| `--scsi` | configure the SCSI disk / enable the `sd0` path |
| `--console-port=N` | serve the interactive session on `127.0.0.1:N` for a VT100/telnet client, leaving the kernel log on stdout |

### Debugging

`--kmsg`, `--trace-traps`, `--pchist` (the last 64K program counters at halt —
the tool that cracked several blockers), `--watch=PC`, `--wtrace=N`,
`--wmem=VA[:len]` and `--pwatch=PA[:len]` (virtual and *physical* write
watchpoints — the second catches a bad translation scribbling over memory its VA
never named), `--profile`, `--vmprobe`, `--ctxtrace`, `--scsitrace`.

Run `./nx88` with no arguments for the full list.

---

## In a browser

[`web/`](web/README.md) builds the whole thing to WebAssembly and puts it in a
page, with xterm.js as the terminal:

```sh
./web/build.sh          # needs emsdk
./web/serve.py          # http://localhost:8000/
```

Embedding is three tags and an element:

```html
<link rel="stylesheet" href="vendor/xterm.css">
<link rel="stylesheet" href="tc2000.css">
<script src="vendor/xterm.js"></script>
<script src="tc2000.js"></script>

<tc2000-console style="height:560px"></tc2000-console>
```

The emulator runs in a Worker and blocks on console reads via Asyncify rather
than SharedArrayBuffer, which means no cross-origin-isolation headers and so no
deployment requirements beyond a static host. It runs at roughly 7 million
instructions/second in wasm against 25 native, and boots in about half a second.

---

## How the emulator is built

`emu/` is a dozen small C files plus a shared `nx88.h`; all machine state and
configuration lives in `globals.c`, and each other file holds one subsystem.

| | |
|---|---|
| `cpu.c` | 88100 decode and execute: delay slots, triadic form, control registers, bit-field ops, `ff0`/`ff1`, the SFU1 FPU, exceptions and `rte` |
| `mmu.c` | 88200 two-level translation with a per-side TLB, the CMMU probe command, and the synthetic boot loader |
| `devices.c` | TCS mailbox, timer, interleaver/CMRAM window, b2vme master mapper, SHA SCSI adapter — modelled just far enough to boot |
| `proc.c` | demand paging, the process lifetime, program loading |
| `sysmode.c` | the system-mode driver loop |
| `console.c` | the terminal: stdio, TCP socket, or the browser, and the 4.3BSD `sgtty` line discipline behind all three |

There is no boot loader — it lived in ROM — so `boot_build_tables()` synthesizes
the segment and page tables the kernel expects to find. The kernel then runs
under genuine translation from early boot, so its own page-table writes land
where it will later look for them.

[`emu/BOOT.md`](emu/BOOT.md) documents the boot arc: ten distinct blockers, each
of which had to be understood before the next became visible. A representative
one — a panic long blamed on the scheduler (`sleep_and_unlock: sleeping idle
proc`) turned out to be a buffer-cache completion gap, `biowait` never seeing
`B_DONE` because synchronous SCSI completion never ran `biodone`.

## How it was worked out

`tools/` is the archaeology, in Python, and it came first. The 88100 decoder was
not written from a manual; it was **inferred from the kernel binary and then
tested against it**:

- `triadic.py` histograms the 11-bit function field over real kernel text and
  pins meanings with positional anchors — the word that most often *ends* a
  function is the return, so that encoding is `jmp r1`.
- `validate.py` checks the result against three independent oracles: what
  fraction of text decodes at all, whether every `bsr` target lands exactly on a
  function symbol, and more.
- `stabs.py` and `frames.py` cross-check the symbol table against the stabs
  debug entries and BBN's own frame descriptors — different parts of the
  toolchain produced them, so where they disagree, one is provably wrong.
- `syscalls.py`, `sysent*.py` recover the complete system-call table from the
  kernel image; `nx-syscalls.txt` is the result.
- `callfn.py` runs kernel functions directly under emulation to recover their
  behaviour rather than deducing it — the address-manipulation family is pure
  bit-twiddling, so it is easier to feed it inputs and read the answers off.
- `sysmode.py` was the reconnaissance harness: log every access that falls
  outside the loaded image, and let the boot path tell you what hardware has to
  exist next.

Once the decoder was trustworthy the emulator was ported to C for speed. The
Python tools remain the reference for the file formats and the instruction set.

---

## Repository layout

```
emu/        the emulator, in C — build.sh, Makefile, BOOT.md
web/        the WebAssembly build and the embeddable page
tools/      the Python archaeology: decoder, disassembler, a.out and
            stabs readers, syscall-table recovery, validation oracles
```

The tape data — `tapeimage.img`, `disk.img` and all the recovered
archives — are not in the repository.

## Status

The kernel boots, mounts its root filesystem, creates real processes through its
own `execve`/`fork`/copy-on-write path, delivers genuine MC88100 access faults,
and runs the tape's shell interactively. Programs beyond the shell run via
`--uprog`, and `--hostfile` plus the SCSI disk model is enough for the guest to
`newfs`, `mount` and populate `disk.img` and then execute binaries out of it.

Note that `emu/BOOT.md` still describes the state of things as of the
root-mount milestone; its closing section on reaching userland has since been
overtaken by the real process work.

---

## Sources

- [BBN Butterfly — Wikipedia](https://en.wikipedia.org/wiki/BBN_Butterfly)
- [The BBN TC2000 — University of Oregon](https://www.cs.uoregon.edu/research/paraducks/papers/sc93.d/subsubsection3_3_2_2.html)
- [The BBN Butterfly family: an overview](https://link.springer.com/content/pdf/10.1007/3-540-57981-8_166.pdf)
- [TC2000 System Software Installation Guide, 1991 (bitsavers)](http://ftpmirror.your.org/pub/misc/bitsavers/pdf/bbn/bbnaci/tc2000/TC2000_Software_Installation_Guide_199107.pdf)
- [Motorola 88100 — Wikipedia](https://en.wikipedia.org/wiki/Motorola_88100)
- [BBN TC2000 panel — Computer History Museum](https://www.computerhistory.org/collections/catalog/102729271)
