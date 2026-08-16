# nx88 in a browser

The MC88100 emulator, compiled to WebAssembly, booting nX off the 1989 install
tape into an interactive `/bin/sh` — inside a page, in a `<div>`.

    ./build.sh          # needs emsdk on PATH
    ./serve.py          # then open http://localhost:8000/

`index.html` is the demo page. Everything else here is the embeddable part.

Every script here anchors itself to this directory, so the working directory
never matters — from the repo root it is just:

    ./web/build.sh
    ./web/serve.py
    node web/selftest.js

---

## Building

The only prerequisite is the Emscripten SDK:

    git clone https://github.com/emscripten-core/emsdk
    cd emsdk && ./emsdk install latest && ./emsdk activate latest
    . ./emsdk_env.sh                      # Windows: emsdk_env.bat

Then:

    ./build.sh                            # or:  ./build.sh --debug

That compiles `emu/*.c` plus `emu/web.c` into `nx88.js` + `nx88.wasm` (~140 KB
of wasm) and stages the two host files the emulator opens into `data/`:

| file | what it is |
|---|---|
| `data/vmunix` | the kernel, loaded as an a.out — 1.2 MB |
| `data/tapeimage.img` | the tape's UFS, read as the root filesystem — 7.5 MB |

`disk.img` is deliberately not staged. It is the 256 MB SCSI install target and
nothing in the shell demo touches it; the emulator runs fine without one.

Both are generated, along with `nx88.js`/`nx88.wasm`, and are gitignored.

## Testing without a browser

    node selftest.js            # boot, run commands, check the output
    node selftest.js -v         # ...and print the session transcript

`selftest.js` plays the part `nx88-worker.js` plays, through the same entry
point and the same I/O hooks, so it exercises the parts of the port that are
easy to get wrong — the ASYNCIFY unwind across a blocking console read, the
input ring, the line discipline — rather than just checking that the module
instantiates. This is why `node` is in the build's `ENVIRONMENT` list.

## Embedding it in a page

Three tags and an element:

```html
<link rel="stylesheet" href="vendor/xterm.css">
<link rel="stylesheet" href="tc2000.css">
<script src="vendor/xterm.js"></script>
<script src="tc2000.js"></script>

<tc2000-console style="height:560px"></tc2000-console>
```

Attributes: `autostart` (boot without the click), `args`, `base`, `font-size`.

Or imperatively, which is what you want if you need the handle:

```js
const machine = TC2000.boot({
  mount: document.querySelector('#machine'),
  autostart: true,
});

machine.send('ls /bin\n');    // type at the guest
machine.eof();                // ^D: halt
machine.reboot();
```

### Options

| option | default | |
|---|---|---|
| `mount` | *(required)* | element to take over |
| `args` | `sys /tapeimage/vmunix --shell --kmsg` | the `nx88` command line, verbatim — every flag in `emu/BOOT.md` works |
| `files` | kernel + tape | `[{path, url, label}]`, staged into the emulator's filesystem before it runs |
| `base` | this script's directory | where `nx88.js`, `nx88-worker.js` and `data/` live |
| `autostart` | `false` | skip the click-to-boot overlay |
| `restartButton` | `true` | the status bar's Restart button; turn off if you supply your own chrome and call `reboot()` yourself |
| `fontSize` | `15` | before scaling |
| `maxScale` | `2.5` | how far the terminal may be blown up to fill its box |
| `theme` | green phosphor | an xterm.js theme object |
| `onStatus`, `onExit` | | callbacks |

`args` is the interesting one. `--kmsg` is on by default because it echoes the
kernel's own console output — the memory sizing, the VMEbus probe, `nX
Operating System (TC2000) #191: Tue Nov 28 18:33:02 1989`. The emulator's own
commentary is off (that is `--debug`), so what the page shows is what the
machine said. To run something other than the shell, say
`sys /tapeimage/vmunix --uprog=/usr/games/snake --kmsg`.

## Deploying

The built page is plain static files: copy this directory to any static host.
There are **no COOP/COEP response headers to arrange**, because the port uses
ASYNCIFY rather than SharedArrayBuffer (see below) — so GitHub Pages and
friends work as-is. `serve.py` exists only for local development; what it adds
over `python -m http.server` is `application/wasm`, `no-store` so a rebuild is
actually picked up, and Range requests for the 7 MB tape image.

`file://` will not work: the page starts a Worker and fetches the images.

Serve the two files in `data/` with compression if you can. The tape image is
mostly empty filesystem and gzips to a fraction of its size.

---

## How it works

    page (tc2000.js)  ──  xterm.js, 80x24, scaled to fit
          │  postMessage: keystrokes down, bytes up
    worker (nx88-worker.js)
          │  ccall
    nx88.wasm  ──  emu/*.c, unchanged except for two guarded additions

The emulator runs in a Worker because it is a straight-line interpreter that
runs flat out and blocks on console reads; on the page's thread that is a
frozen tab.

**The browser gets the SOCKET console, not the stdio one.** A terminal on a
page sends raw, unechoed keystrokes, exactly as the telnet client that
`--console-port` serves does — so every part of the line discipline a host tty
would have provided (echo, backspace, CR/CRLF→LF, ONLCR outbound) has to happen
in the emulator. `console.c` therefore does not grow a third transport: the web
build *is* the socket transport with its two bottom-most primitives swapped.
`con_cook_line`, the sgtty raw/cbreak switch and `FIONREAD` are shared
unmodified, which is what makes the curses programs behave the same in a tab as
they do over telnet.

**Blocking reads use ASYNCIFY, not SharedArrayBuffer.** A guest `read()` with
nothing typed yet has to wait, and a browser tab cannot spin — the event loop
it would block is the one that delivers the keystroke. `emscripten_sleep()`
unwinds the whole C stack back to the worker's event loop and rewinds it when a
key lands. That choice is what removes the cross-origin-isolation requirement.
It is cheap here for a specific reason: the syscall dispatch lives in `run_sys`
and not inside `step()`, and the emulator has no function pointers anywhere, so
emcc's call-graph analysis is exact and the instrumented set stops one frame
above the interpreter's hot loop. Measured: ~7 Minsn/s in wasm against ~24
native, and boot-to-prompt is about half a second.

The C changes are all `#ifdef __EMSCRIPTEN__` and the native build is
untouched:

| file | change |
|---|---|
| `emu/console.c` | the web transport — output hook, input ring, and the four primitives that switch on it |
| `emu/sysmode.c` | one periodic `emscripten_sleep(0)` in the run loop, so the boot log streams instead of arriving in a lump |
| `emu/web.c` | *new*, web build only: `nx_web_run`, the entry point the page calls instead of `main` |

`nx_web_run` exists because `callMain` cannot be awaited once ASYNCIFY is in
play: `main()` unwinds on the first console read and returns to JS long before
the machine has stopped, so the exit code and the halt moment are both lost.
Going through an ordinary exported function lets the worker use
`ccall(..., {async: true})`, which yields a promise that settles when `main()`
really returns.

## Notes

* The terminal is **fixed at 80×24** and scaled to fit its box rather than
  reflowed. That is not a style choice: the emulator answers the guest's
  `TIOCGWINSZ` with a hard-coded 24×80 and `/etc/termcap` describes a vt100, so
  a terminal of any other size would have the curses programs drawing to
  coordinates that are not where the text is. xterm's DOM renderer draws real
  text, so a CSS transform re-rasterises rather than blurring.
* **RETURN is a bare CR on this wire**, and the cooked-line path had to learn
  that. A telnet client encodes a line ending as CR LF or CR NUL (RFC 854), so
  `con_cook_line` reads one more byte after a CR to absorb the pair — and over
  a socket it *must*, since treating a stray LF as an empty line reads as EOF
  and kills the shell. xterm sends one byte per keystroke, so that read blocked:
  Enter appeared to do nothing until the next key was pressed, which then
  delivered both. On the page's wire the peek is conditional on a byte already
  being in hand. If you write another frontend, this is the trap.
* **Ctrl-D halts the machine; `exit` does not.** This 1989 `/bin/sh` runs
  `exit` and carries straight on — verified against the native binary, where
  input after `exit` still executes. End-of-file is the only thing that stops
  it, in the browser and natively alike. The status bar's **Restart** button
  reboots from cold at any time, halted or not.
* **A reboot builds a new Worker, but does not re-download.** The machine state
  is a page of C globals with no teardown path, so a fresh worker is the only
  guaranteed-clean machine — but the page keeps the disk images from the first
  boot and hands each new worker a copy, so a restart costs a boot and not 9 MB.
  One consequence for development: after `./build.sh`, Restart picks up the new
  `nx88.wasm` but keeps the old images in memory. Reload the page if you
  re-extracted the tape.
* A reboot throws the worker away and starts a new one. The emulator's machine
  state is a page of C globals with no teardown path, and a fresh worker is a
  guaranteed-clean machine for the price of re-instantiating 140 KB.
* **What scrolls past is the kernel's own log, not the emulator's.** The
  emulator's commentary — load map, synthetic boot tables, the proc
  experiment's register dumps, `[kfall]`, `[halt]` — is behind `--debug` (see
  `emu/BOOT.md`), which is why the page passes `--kmsg` and not `--debug`. Add
  `--debug` to `args` when something goes wrong and you want the machine's
  internals back. `selftest.js` asserts both halves: that the kernel's log
  arrives, and that none of the commentary leaks without the flag.

### Vendored files

`vendor/` holds xterm.js so the page has no build step and no CDN dependency.
To refresh:

    curl -sSLo vendor/xterm.js  https://cdn.jsdelivr.net/npm/@xterm/xterm@5.5.0/lib/xterm.js
    curl -sSLo vendor/xterm.css https://cdn.jsdelivr.net/npm/@xterm/xterm@5.5.0/css/xterm.css
