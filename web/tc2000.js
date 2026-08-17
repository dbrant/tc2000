/* tc2000.js -- drop the TC2000 into a page.
 *
 *   <link rel="stylesheet" href="vendor/xterm.css">
 *   <link rel="stylesheet" href="tc2000.css">
 *   <script src="vendor/xterm.js"></script>
 *   <script src="tc2000.js"></script>
 *
 *   <tc2000-console style="height:520px"></tc2000-console>
 *
 * or, imperatively:
 *
 *   const s = TC2000.boot({ mount: document.querySelector('#machine') });
 *   s.start();
 *
 * Everything heavy happens in a Worker (nx88-worker.js); this file is the
 * terminal, the chrome and the wiring.
 */
(function (global) {
  'use strict';

  /* Where nx88.js, nx88-worker.js and data/ live.  Taken from this script's own
     URL so the page can sit in a different directory from the emulator. */
  const SELF = (document.currentScript && document.currentScript.src) ||
               new URL('tc2000.js', location.href).href;
  const BASE = new URL('.', SELF).href;

  /* ★ 80x24, ALWAYS.  Not a style choice: the emulator answers the guest's
     TIOCGWINSZ with a hard-coded 24x80 (console.c), and /etc/termcap on the
     tape describes a vt100.  A terminal of any other size would have the
     curses programs -- vi, the games -- drawing to coordinates that are not
     where the text is.  So the geometry is fixed and the WHOLE terminal is
     scaled to fit whatever box it is given, rather than reflowed. */
  const COLS = 80, ROWS = 24;

  const DEFAULTS = {
    /* One file: the tape image is a filesystem that CONTAINS the kernel, and
       the emulator reads /vmunix out of it rather than being handed a separate
       copy -- so there is nothing else to download.
       --kmsg is the point of the demo: it echoes the kernel's OWN console
       output ("nX Operating System (TC2000) #191: Tue Nov 28 18:33:02 1989",
       the memory sizing, the VMEbus probe) rather than only the emulator's
       commentary about it. */
    args: 'sys /boot.img --shell --kmsg --clock',
    files: [
      { path: '/boot.img', url: 'data/boot.img', label: 'boot image' },
    ],
    autostart: false,
    fontSize: 15,
    maxScale: 2.5,
    theme: {
      background: '#0d0f0e',
      foreground: '#c8f0c0',
      cursor:     '#7dff7d',
      selectionBackground: '#2d5a2d',
      black: '#0d0f0e', red: '#e06c5a', green: '#8fdf7a', yellow: '#e6c86a',
      blue: '#6aa9e6', magenta: '#c08fe6', cyan: '#6ae0d0', white: '#c8f0c0',
    },
  };

  const enc = new TextEncoder();

  function el(tag, cls, parent) {
    const n = document.createElement(tag);
    if (cls) n.className = cls;
    if (parent) parent.appendChild(n);
    return n;
  }

  function boot(userOpts) {
    const opts = Object.assign({}, DEFAULTS, userOpts || {});
    const base = opts.base ? new URL(opts.base, location.href).href : BASE;
    const mount = opts.mount;
    if (!mount) throw new Error('TC2000.boot: no mount element');
    if (typeof global.Terminal !== 'function')
      throw new Error('TC2000.boot: xterm.js is not loaded (vendor/xterm.js)');

    /* ------------------------------------------------------------- chrome */
    mount.classList.add('tc2000');
    mount.innerHTML = '';
    const screen = el('div', 'tc2000-screen', mount);
    const stage  = el('div', 'tc2000-stage', screen);
    const host   = el('div', 'tc2000-term', stage);
    const bar    = el('div', 'tc2000-bar', mount);
    const lamp   = el('span', 'tc2000-lamp', bar);
    const label  = el('span', 'tc2000-label', bar);
    const meter  = el('span', 'tc2000-meter', bar);
    const fill   = el('i', '', meter);
    const restart = opts.restartButton === false ? null
                                                 : el('button', 'tc2000-restart', bar);
    const overlay = el('div', 'tc2000-overlay', screen);

    label.textContent = 'BBN TC2000 — nX, 1989';

    if (restart) {
      restart.type = 'button';                  /* not submit: it may sit in a form */
      restart.textContent = 'Restart';
      restart.title = 'Reboot the machine from cold';
      restart.addEventListener('click', () => {
        /* Hand the keyboard straight back.  Without this the button keeps
           focus through the reboot and the first thing typed at the new shell
           goes to the button instead of the machine -- which, since Space and
           Enter activate a focused button, means an accidental second reboot. */
        restart.blur();
        start();
      });
    }

    /* ------------------------------------------------------------ terminal */
    const term = new global.Terminal({
      cols: COLS, rows: ROWS,
      fontSize: opts.fontSize,
      fontFamily: opts.fontFamily ||
        'ui-monospace, SFMono-Regular, "SF Mono", Menlo, Consolas, monospace',
      theme: opts.theme,
      cursorBlink: true,
      convertEol: false,      /* the guest's ONLCR already sends CR LF */
      scrollback: 5000,
    });
    term.open(host);

    /* Scale the whole 80x24 terminal to fill the box.  xterm's DOM renderer
       draws real text, so a CSS transform re-rasterises rather than blurring --
       which is what makes fixing the geometry affordable in the first place. */
    function refit() {
      const w = stage.offsetWidth, h = stage.offsetHeight;
      if (!w || !h) return;
      const k = Math.min(screen.clientWidth / w, screen.clientHeight / h,
                         opts.maxScale);
      stage.style.transform = `translate(-50%, -50%) scale(${k})`;
    }
    const ro = new ResizeObserver(refit);
    ro.observe(screen);
    requestAnimationFrame(refit);

    /* --------------------------------------------------------------- state */
    let worker = null, running = false, started = false;
    /* The disk images, kept across reboots.  The worker gives them up once it
       has copied them into the emulator's filesystem, and every subsequent
       worker is handed a clone instead of downloading them again. */
    let images = null;

    function setPhase(phase, text, loaded, total) {
      mount.dataset.phase = phase;
      if (text) label.textContent = text;
      if (total > 0) {
        meter.style.display = '';
        fill.style.width = Math.round((loaded / total) * 100) + '%';
      } else {
        meter.style.display = 'none';
      }
      if (opts.onStatus) opts.onStatus(phase, text);
    }

    function showOverlay(title, note, buttonText, onClick) {
      overlay.innerHTML = '';
      overlay.style.display = 'flex';
      el('div', 'tc2000-overlay-title', overlay).textContent = title;
      if (note) el('div', 'tc2000-overlay-note', overlay).textContent = note;
      if (buttonText) {
        const b = el('button', 'tc2000-button', overlay);
        b.textContent = buttonText;
        b.addEventListener('click', onClick);
      }
    }
    function hideOverlay() { overlay.style.display = 'none'; overlay.innerHTML = ''; }

    function stop() {
      if (worker) { worker.terminate(); worker = null; }
      running = false;
    }

    /* A reboot throws the worker away and makes a new one.  The emulator's
       machine state is a page of C globals with no teardown path -- running it
       twice in one instance would start the second boot on the first one's
       memory map -- and a fresh worker is a guaranteed-clean machine for the
       price of re-instantiating a 140 KB module. */
    function start() {
      stop();
      started = true;
      term.reset();
      hideOverlay();
      setPhase('loading', 'Loading emulator');

      worker = new Worker(new URL('nx88-worker.js', base).href);
      worker.onmessage = (e) => {
        const m = e.data;
        switch (m.type) {
          case 'out':
            term.write(m.data);
            break;
          case 'images':
            images = m.files;
            break;
          case 'status':
            if (m.phase === 'running' && !running) {
              running = true;
              setPhase('running', 'Booting nX — MC88100 @ node 0');
              term.focus();
            } else if (m.phase === 'shell') {
              setPhase('running', m.text || 'nX — /bin/sh');
            } else if (m.phase === 'loading') {
              setPhase('loading', m.text, m.loaded, m.total);
            }
            break;
          case 'exit':
            running = false;
            setPhase('halted', 'Machine halted');
            showOverlay('Machine halted',
                        m.code ? `nx88 exited with status ${m.code}.`
                               : 'The shell exited and the machine stopped.',
                        'Reboot', start);
            if (opts.onExit) opts.onExit(m.code);
            break;
          case 'error':
            term.write('\r\n\x1b[31m[' + m.text + ']\x1b[0m\r\n');
            break;
        }
      };
      worker.onerror = (e) => {
        setPhase('halted', 'Failed to start');
        showOverlay('Could not start', e.message || String(e), 'Try again', start);
      };
      /* `images` is sent WITHOUT a transfer list, so the structured clone
         leaves the page's own copy intact and the machine can be rebooted
         any number of times. */
      worker.postMessage({ type: 'start', args: opts.args, files: opts.files,
                           base, preloaded: images });
    }

    /* Keystrokes.  xterm hands us the terminal's own encoding of the key --
       an escape sequence for the arrows, \x04 for Ctrl-D -- which is exactly
       what a serial line would carry, so it goes straight across. */
    term.onData((s) => {
      if (worker) worker.postMessage({ type: 'input', data: enc.encode(s) });
    });
    term.onBinary((s) => {
      if (!worker) return;
      const b = new Uint8Array(s.length);
      for (let i = 0; i < s.length; i++) b[i] = s.charCodeAt(i) & 0xff;
      worker.postMessage({ type: 'input', data: b });
    });
    screen.addEventListener('mousedown', () => { if (running) term.focus(); });

    if (opts.autostart) start();
    else {
      setPhase('idle', 'Ready');
      showOverlay('BBN TC2000',
                  'Boot nX from the 1989 install tape. About 9 MB to download.',
                  'Boot the machine', start);
    }

    return {
      term,
      start,
      stop,
      reboot: start,
      get running() { return running; },
      get started() { return started; },
      /* Feed the guest as if it had been typed -- handy for scripted demos. */
      send(text) {
        if (worker) worker.postMessage({ type: 'input', data: enc.encode(text) });
      },
      eof() { if (worker) worker.postMessage({ type: 'eof' }); },
      destroy() { stop(); ro.disconnect(); term.dispose(); mount.innerHTML = ''; },
    };
  }

  /* The declarative form.  Attributes mirror the option names. */
  class TC2000Console extends HTMLElement {
    connectedCallback() {
      if (this._session) return;
      const o = { mount: this, autostart: this.hasAttribute('autostart') };
      if (this.hasAttribute('args')) o.args = this.getAttribute('args');
      if (this.hasAttribute('base')) o.base = this.getAttribute('base');
      if (this.hasAttribute('font-size'))
        o.fontSize = Number(this.getAttribute('font-size'));
      this._session = boot(o);
    }
    disconnectedCallback() {
      if (this._session) { this._session.destroy(); this._session = null; }
    }
    get session() { return this._session; }
  }
  if (!customElements.get('tc2000-console'))
    customElements.define('tc2000-console', TC2000Console);

  global.TC2000 = { boot, COLS, ROWS, base: BASE };
})(window);
