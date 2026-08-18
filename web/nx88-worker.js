/* nx88-worker.js -- the emulator's thread.
 *
 * The machine runs here rather than on the page's thread for one reason: it is
 * a straight-line interpreter that runs flat out and blocks on console reads,
 * and a page that did that would be a frozen tab.  Down here it can block all
 * it likes, and the only thing between it and the terminal is postMessage.
 *
 * Protocol, page -> worker:
 *   {type:'start', args, files, base}   fetch the images, then run
 *   {type:'input', data:Uint8Array}     keystrokes
 *   {type:'eof'}                        ^D at the transport level: end the session
 * worker -> page:
 *   {type:'status', phase, text, loaded, total}
 *   {type:'out', data:Uint8Array}       bytes for the terminal
 *   {type:'exit', code}                 main() returned; the machine has halted
 *   {type:'error', text}
 */
'use strict';

/* ------------------------------------------------------------------ output */
/* Every byte the machine emits arrives here: the kernel's boot log through
   Emscripten's stdout device, and the guest's own console writes through the
   EM_JS hook in console.c.  Both land in the same queue in call order, which is
   what keeps the boot log and the shell session interleaved exactly as they are
   on a real serial console.
   Batching matters: a message per byte would be tens of thousands of them for
   the boot log alone.  Flushing on a timer piggybacks on the fact that timers
   in this worker can only run when the emulator yields (see the ASYNCIFY note
   in emu/console.c), so each flush naturally carries one burst of output. */
let outBuf = [];
let outTimer = 0;

function flushOut() {
  outTimer = 0;
  if (!outBuf.length) return;
  const data = new Uint8Array(outBuf);
  outBuf = [];
  postMessage({ type: 'out', data }, [data.buffer]);
}

function outByte(b) {
  if (b === null || b === undefined) return;   // device close / flush marker
  outBuf.push(b);
  if (outBuf.length >= 8192) flushOut();
  else if (!outTimer) outTimer = setTimeout(flushOut, 0);
}

/* The two streams arrive with DIFFERENT line endings, and a terminal is
   unforgiving about it.  The guest's console output is already cooked: it goes
   through the emulator's ONLCR in con_sock_write, so its newlines are CR LF.
   The kernel log does not -- it is ordinary printf on stdout, and on a real
   machine the host tty is what turns its bare LF into CR LF.  There is no tty
   here, and a terminal given a bare LF moves DOWN a line without returning to
   column 0, so an uncooked boot log comes out as a staircase marching off the
   right edge.  Cook it on the way past, and only here: doing it in nxWebOut as
   well would double the CRs the guest deliberately sent. */
let lastCooked = 0;
function outCooked(b) {
  if (b === null || b === undefined) return;
  if (b === 10 && lastCooked !== 13) outByte(13);
  lastCooked = b;
  outByte(b);
}

/* The sink nx_web_out() in emu/console.c calls.  It has to be a global: EM_JS
   bodies run inside the Emscripten module's scope, which is nested in this
   worker's, so a plain name resolves out to here.
   Its FIRST call is a milestone worth reporting: only the guest's own console
   comes through here, so it means the kernel has finished booting and handed
   the terminal to /bin/sh.  Nothing else on the page can tell boot output from
   session output once both are bytes in the same stream. */
let shellUp = false;
globalThis.nxWebOut = function (bytes) {
  if (!shellUp && bytes.length) {
    shellUp = true;
    status('shell', 'nX — /bin/sh');
  }
  for (let i = 0; i < bytes.length; i++) outByte(bytes[i]);
};

/* ------------------------------------------------------------------- input */
let Module = null;
const pendingInput = [];

function pushInput(bytes) {
  if (!Module) { pendingInput.push(bytes); return; }
  Module.ccall('nx_web_push', null, ['array', 'number'], [bytes, bytes.length]);
}

/* ------------------------------------------------------------------ loading */
function status(phase, text, loaded, total) {
  postMessage({ type: 'status', phase, text, loaded, total });
}

/* Fetch with progress, because the tape image is 7 MB and a demo that shows
   nothing for the length of that download reads as broken. */
async function fetchWithProgress(url, onProgress) {
  const res = await fetch(url);
  if (!res.ok) throw new Error(`${url}: HTTP ${res.status}`);
  const total = Number(res.headers.get('content-length')) || 0;
  if (!res.body) return new Uint8Array(await res.arrayBuffer());

  const reader = res.body.getReader();
  const chunks = [];
  let loaded = 0;
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    chunks.push(value);
    loaded += value.length;
    onProgress(loaded, total);
  }
  const out = new Uint8Array(loaded);
  let off = 0;
  for (const c of chunks) { out.set(c, off); off += c.length; }
  return out;
}

async function start(msg) {
  status('loading', 'Loading emulator');

  Module = await createNx88({
    noInitialRun: true,
    /* Per-BYTE stdout/stderr, not Emscripten's default per-line print().  The
       kernel log is mostly whole lines, but not all of it is -- the --kmsg
       character drain in devices.c emits one character at a time -- and a
       prompt that never ends in a newline must not sit in a buffer. */
    stdout: outCooked,
    stderr: outCooked,
    printErr: (t) => postMessage({ type: 'error', text: String(t) }),
    locateFile: (p) => new URL(p, msg.base).href,
  });

  /* The images go into Emscripten's in-memory filesystem at the paths the
     emulator expects.  It derives the root image's name from the kernel's:
     <dir>/vmunix -> <dir>.img (see main.c), so these two names are not
     independent. */
  const stage = (path, bytes) => {
    const dir = path.slice(0, path.lastIndexOf('/'));
    if (dir) try { Module.FS.mkdirTree(dir); } catch (e) { /* already there */ }
    Module.FS.writeFile(path, bytes);
  };

  if (msg.preloaded) {
    /* A reboot.  The page kept the images from the first boot, so there is
       nothing to download -- which matters because the Restart button invites
       being pressed repeatedly, and otherwise every press would re-fetch 9 MB
       and be at the mercy of whatever cache headers the host happens to send. */
    status('loading', 'Restoring disk images');
    for (const f of msg.preloaded) stage(f.path, f.bytes);
  } else {
    const files = msg.files;
    const keep = [];
    for (let i = 0; i < files.length; i++) {
      const f = files[i];
      const label = `${f.label || f.path} (${i + 1}/${files.length})`;
      const url = new URL(f.url, msg.base).href;
      /* Progress is reported per file rather than against a grand total: the
         totals are whatever the server's content-length says, and hard-coding
         them here would go stale the moment someone points the page at a
         different tape. */
      const bytes = await fetchWithProgress(url, (loaded, total) => {
        status('loading', `Loading ${label}`, loaded, total);
      });
      stage(f.path, bytes);
      keep.push({ path: f.path, bytes });
    }
    /* Hand them to the page for next time.  Transferring rather than copying is
       safe: FS.writeFile has already copied every byte into the wasm heap, so
       this worker has no further use for the buffers. */
    postMessage({ type: 'images', files: keep }, keep.map((f) => f.bytes.buffer));
  }

  for (const b of pendingInput) pushInput(b);
  pendingInput.length = 0;

  status('running', 'Booting');

  /* ccall with {async:true} rather than callMain: see the comment on
     nx_web_run in emu/web.c for why the entry point is not main() itself.
     This promise settles when the machine has actually halted. */
  let code = 0;
  try {
    code = await Module.ccall('nx_web_run', 'number', ['string'], [msg.args],
                              { async: true });
  } catch (e) {
    postMessage({ type: 'error', text: String(e && e.message || e) });
  }
  flushOut();
  postMessage({ type: 'exit', code });
}

/* --------------------------------------------------------------- dispatch */
self.onmessage = (e) => {
  const m = e.data;
  switch (m.type) {
    case 'start':
      importScripts(new URL('nx88.js', m.base).href);   // defines createNx88
      start(m).catch((err) => {
        postMessage({ type: 'error', text: String(err && err.message || err) });
        postMessage({ type: 'exit', code: -1 });
      });
      break;
    case 'input':
      pushInput(m.data);
      break;
    case 'eof':
      if (Module) Module.ccall('nx_web_eof');
      break;
  }
};
