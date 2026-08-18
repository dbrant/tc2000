#!/usr/bin/env node
/* selftest.js -- drive the WebAssembly build head-less, with no browser.
 *
 *   node selftest.js            boot, run a few commands, check the output
 *   node selftest.js -v         also print the whole session transcript
 *
 * This stands in for the page: it plays the part nx88-worker.js plays, using
 * the same entry point (nx_web_run), the same output hook (globalThis.nxWebOut)
 * and the same input path (nx_web_push).  So it exercises the parts of the port
 * that are easy to get wrong -- the ASYNCIFY unwind across a blocking console
 * read, the ring buffer, the line discipline -- rather than just checking that
 * the module instantiates.
 */
'use strict';
const fs = require('fs');
const path = require('path');
const createNx88 = require('./nx88.js');

const verbose = process.argv.includes('-v');

/* The page's own command line by default.  NX_ARGS overrides it, which is how
   the other side of the --debug gate gets tested:
     NX_ARGS='sys /boot.img --shell --kmsg --debug' node selftest.js
   flips the "emulator stayed quiet" checks into "the commentary came back". */
const ARGS = process.env.NX_ARGS || 'sys /boot.img --shell --kmsg --color';
const emuDebug = /(^|\s)--debug(\s|$)/.test(ARGS);

/* The session, as the terminal would see it. */
let transcript = '';
function sink(bytes) {
  const s = Buffer.from(bytes).toString('latin1');
  transcript += s;
  if (verbose) process.stdout.write(s);
}
globalThis.nxWebOut = sink;

/* What to type; each line is fed only once the guest asks for input, see the
   pump below.

   Lines end in CR, not LF, because that is what the browser sends: xterm
   emits one byte per keystroke and RETURN is a bare \r.  LF would skip the
   CR-pairing peek in con_cook_line entirely and test a path no user can reach.
   Each line is pushed as one batch too, so the CR is the last byte in the
   ring -- the case where that peek must not block.

   The last entry is Ctrl-D, not `exit': this 1989 /bin/sh runs `exit' and
   carries on, input after it still executing, so end-of-file is the only thing
   that stops the machine. */
const SCRIPT = [
  'echo hello from 1989\r',
  'ls /bin\r',
  'pwd\r',
  '\x04',
];

(async function main() {
  const Module = await createNx88({
    noInitialRun: true,
    stdout: (b) => { if (b !== null) sink([b]); },
    stderr: (b) => { if (b !== null) sink([b]); },
  });

  /* The very file the page fetches, at the very path the worker stages it to.
     Just the one: the kernel lives inside this filesystem and the emulator
     reads it out of there. */
  for (const [src, dst] of [[path.join(__dirname, 'data', 'boot.img'), '/boot.img']]) {
    if (!fs.existsSync(src)) {
      console.error(`missing ${src} -- run ./build.sh first`);
      process.exit(2);
    }
    const dir = dst.slice(0, dst.lastIndexOf('/'));
    if (dir) try { Module.FS.mkdirTree(dir); } catch (e) { /* exists */ }
    Module.FS.writeFile(dst, new Uint8Array(fs.readFileSync(src)));
  }

  /* Type the next line only once the PROMPT is back, the way a person does.
     Waiting for the machine to merely go quiet is not good enough, and quietly
     hid a real bug: when a RETURN failed to be delivered, the next keystroke
     this pump sent was itself what unblocked the stuck read, so every command
     still ran and the transcript came out identical.  Requiring the prompt
     means a swallowed RETURN stalls here and fails the run, which is the whole
     point of the test. */
  const STALL_TICKS = 50;             /* x 60 ms = 3 s of silence with no prompt */
  let next = 0, lastLen = -1, idle = 0, failed = null;
  const pump = setInterval(() => {
    if (next >= SCRIPT.length) return;
    if (!/# $/.test(transcript)) {
      if (transcript.length !== lastLen) { lastLen = transcript.length; idle = 0; return; }
      if (++idle > STALL_TICKS) {
        failed = `no prompt before script step ${next + 1} (${JSON.stringify(SCRIPT[next])}); ` +
                 `session tail: ${JSON.stringify(transcript.slice(-90))}`;
        clearInterval(pump);
        Module.ccall('nx_web_eof');   /* let the run finish so we can report */
      }
      return;
    }
    idle = 0;
    const b = Buffer.from(SCRIPT[next++], 'latin1');
    Module.ccall('nx_web_push', null, ['array', 'number'], [b, b.length]);
  }, 60);

  /* A hang must fail loudly rather than sit there: without this, a machine that
     never halts just wedges the process and looks like a slow test. */
  let halted = false;
  const watchdog = setTimeout(() => {
    console.log('  FAIL  machine never halted (watchdog)');
    console.log('\n--- transcript tail ---\n' + transcript.slice(-2000));
    process.exit(1);
  }, 90000);

  const t0 = Date.now();
  const rc = await Module.ccall('nx_web_run', 'number', ['string'], [ARGS],
                                { async: true });
  halted = true;
  clearTimeout(watchdog);
  clearInterval(pump);
  const secs = ((Date.now() - t0) / 1000).toFixed(1);

  /* ---------------------------------------------------------------- checks */
  /* What the machine itself says must arrive... */
  const checks = [
    ['kernel log reaches the terminal', /\[nx\] nX Operating System \(TC2000\)/],
    ['kernel sized its memory',         /\[nx\] Physical memory = 16\.00 megabytes/],
    ['kernel probed the VMEbus',        /\[nx\] Probing for VMEbus/],
    ['shell banner',                    /nX on the TC2000/],
    ['echo ran',                        /hello from 1989/],
    ['ls ran',                          /\bchmod\b[\s\S]*\becho\b/],
    ['pwd ran',                         /^\/\s*$/m],
  ];
  /* ...and what the EMULATOR has to say about it must not, absent --debug.
     This is the half that would otherwise rot silently: a diagnostic added
     later without a dbg() wrapper would go straight to everyone's terminal,
     and nothing else here would notice.  With --debug in ARGS the same list is
     checked the other way up, so the flag is pinned down in both directions. */
  const commentary = [
    ['no load map',            /kernel: text \d+ @ phys/],
    ['no synthetic-boot dump', /synthetic boot:|synthetic free list/],
    ['no boot-complete note',  /\[boot-complete\]/],
    ['no proc experiment',     /=== proc experiment/],
    ['no [kfall] chatter',     /\[kfall\]/],
    ['no halt report',         /\[halt\]/],
    ['no closing summary',     /stopped at pc=/],
  ];

  let bad = 0;
  if (failed) { bad++; console.log(`  FAIL  input stalled: ${failed}`); }
  if (!halted) { bad++; console.log('  FAIL  machine never halted'); }
  for (const [name, re] of checks) {
    const ok = re.test(transcript);
    if (!ok) bad++;
    console.log(`  ${ok ? 'ok  ' : 'FAIL'}  ${name}`);
  }
  for (const [name, re] of commentary) {
    const m = transcript.match(re);
    const ok = emuDebug ? !!m : !m;
    if (!ok) bad++;
    const label = emuDebug ? name.replace(/^no /, '--debug restores ') : name;
    console.log(`  ${ok ? 'ok  ' : 'FAIL'}  ${label}` +
                (!ok && m ? ` -- leaked ${JSON.stringify(m[0])}` : ''));
  }
  console.log(`\n  ${transcript.length} bytes of session in ${secs}s, exit ${rc}`);
  if (bad && !verbose) {
    console.log('\n--- transcript ---\n' + transcript.slice(-3000));
  }
  process.exit(bad ? 1 : 0);
})().catch((e) => { console.error(e); process.exit(1); });
