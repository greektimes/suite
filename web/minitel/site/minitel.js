/*
 * minitel.js - the web Minitel terminal: canvas, keys and the socket.
 *
 * Part of the Montreal Greek Times Unicorn Suite.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * Everything below the wire is the Suite's own engine, compiled to
 * WebAssembly unchanged (engine.js / engine.wasm, built by
 * web/minitel/build.bat from src/minitel_decode.c, src/minitel_screen.c
 * and the fonts; clean-room from STUM1B). This file is the browser side
 * of what src/minitel_view.c and src/naplps_module.c do in the Suite's
 * Videotex tab, and it follows them rule for rule:
 *
 *   - Function keys are SEP (0x13) and a letter (STUM1B table 2-3-6);
 *     letters go up as CAPITALS (p110); NO local echo, the server echoes.
 *   - PC keys as the tab (and MiniPavi's emulator, as measured): Enter or
 *     Right ENVOI, Up RETOUR, F2 REPETITION, F1 GUIDE, Escape or Left
 *     ANNULATION, Home SOMMAIRE, Backspace CORRECTION, Tab or Down SUITE,
 *     F9 CONNEXION/FIN.
 *   - CONNEXION/FIN connects while disconnected; connected, one press
 *     sends SEP I and a second press in a row hangs up (p33-34).
 *   - Logoff: if the HOST closed and bytes arrived after the last thing
 *     sent, the last within 3 s of the close, that is the service's
 *     goodbye and it stays; otherwise "Disconnected from <host>" is drawn
 *     as a Minitel page. 5 s later the start screen comes back; a new
 *     connection cancels that.
 *
 * TRANSPORT. A browser cannot open TCP, so this talks to the estate's
 * own MiniPavi gateway at exactly wss://videotex.greektimes.ca/ws (no
 * query string: nginx refuses one; no trailing slash: /ws/ is 404). The
 * gateway speaks TEXT frames of 7-bit bytes both ways (observed,
 * docs/2026-09-27_WEB_MINITEL_W1.md).
 */
(function () {
  'use strict';

  var WS_URL = 'wss://videotex.greektimes.ca/ws';
  var HOST = 'videotex.greektimes.ca';
  var LOGOFF_MS = 5000;     /* NAP_LOGOFF_MS  */
  var GOODBYE_MS = 3000;    /* NAP_GOODBYE_MS */
  var BLINK_MS = 500;       /* MTL_BLINK_MS: flash 0.5 Hz, cursor every tick */

  /*
   * DISPLAY PACING (operator, 2026-09-27). Our service paints at 120 B/s
   * in 4-byte chunks at 30 Hz, and on port 516 the Suite's tab draws them
   * as they come, so a page builds character by character, as on a
   * Minitel at 1200 baud (120 characters a second). The gateway re-batches
   * that stream into 28-byte websocket frames every 233 ms, which drawn
   * as they arrive show whole words at once. So arriving bytes are queued
   * and fed to the decoder at the same 4 bytes per 1/30 s, one repaint
   * each. A backlog beyond one second (the gateway's own 81-byte status
   * line, a tab that was hidden) is caught up faster rather than let the
   * screen fall behind the line.
   */
  var PACE_HZ = 30, PACE_BPS = 120, PACE_BACKLOG = 120;

  /*
   * THE SERVER'S GOODBYE ON THE WEB ROUTE (operator, 2026-09-27). On port
   * 516 the service says goodbye and closes the line. Through the gateway
   * the line stays open: when the service ends the call (idle, 9 then 3,
   * CONNEXION/FIN) the gateway sends an EMPTY text frame and then its own
   * thank-you page, and keeps the reader there. Observed: every empty
   * frame of the W1 run came 3-23 ms before that page, each at the
   * gateway's logged end of call, and none anywhere else. So an empty
   * frame is taken as "the service said goodbye": what follows is drawn
   * at the usual pace, and once the line has been quiet for
   * GOODBYE_QUIET_MS the page hangs up itself, keeps the goodbye on the
   * screen, and shows the start screen 5 s later, as the Suite does.
   */
  var GOODBYE_QUIET_MS = 600;

  /* minitel_screen.h */
  var R_MONO = 1, R_FLASH = 2, R_CURSOR = 4;
  var P_SCANLINES = 1, P_MONO = 2;

  var SEP_NAME = {
    A: 'ENVOI', B: 'RETOUR', C: 'REPETITION', D: 'GUIDE', E: 'ANNULATION',
    F: 'SOMMAIRE', G: 'CORRECTION', H: 'SUITE', I: 'CONNEXION/FIN'
  };

  /* PC keys (KeyboardEvent.key) to SEP letters. */
  var PC_KEYS = {
    Enter: 'A', ArrowRight: 'A',
    ArrowUp: 'B',
    F2: 'C',
    F1: 'D',
    Escape: 'E', ArrowLeft: 'E',
    Home: 'F',
    Backspace: 'G',
    Tab: 'H', ArrowDown: 'H',
    F9: 'I'
  };

  function $(id) { return document.getElementById(id); }
  var term = $('term'), canvas = $('screen'), wrap = $('screenWrap');
  var startBtn = $('start'), startGo = $('startGo'), startHint = $('startHint');
  var statusText = $('statusText'), countsEl = $('counts');
  var optScan = $('optScan'), optMono = $('optMono');
  var ctx = canvas.getContext('2d', { alpha: false });

  var E = null;              /* the engine module */
  var ws = null;
  var state = 'idle';        /* idle | connecting | connected */
  var opened = false;
  var lastWasCnx = false;
  var rxSinceTx = false, lastRx = 0;
  var logoffTimer = 0;
  var nonByte = 0;           /* text-frame characters above 0xFF */
  var scale = 1, tick = 0, drawQueued = false;
  var rxQueue = [], rxQueued = 0, paceCredit = 0, paceLast = 0;
  var hostClosedAt = 0;      /* the host closed while bytes were still queued */
  var connectWhenReady = false;   /* CONNEXION/FIN pressed before the engine loaded */
  var goodbyeSeen = false, goodbyeTimer = 0;

  /* Per-viewer conveniences only; the page works without storage. */
  function prefGet(k, dflt) {
    try { var v = window.localStorage.getItem('mtw.' + k); return v === null ? dflt : v === '1'; }
    catch (e) { return dflt; }
  }
  function prefSet(k, v) {
    try { window.localStorage.setItem('mtw.' + k, v ? '1' : '0'); } catch (e) { /* ignore */ }
  }

  /* Scanlines default ON (the Suite's default). Monochrome default ON:
   * the Suite's Minitel default, and the service's pages are authored for
   * the Minitel 1B's grey tube (STUM1B p32); colour is one tap away. */
  var scan = prefGet('scan', true);
  var mono = prefGet('mono', true);
  optScan.checked = scan;
  optMono.checked = mono;

  function setStatus(s) { statusText.textContent = s; }

  function focusScreen() {
    try { canvas.focus({ preventScroll: true }); } catch (e) { canvas.focus(); }
  }

  /* ---------------------------------------------------------------- */
  /* Drawing                                                          */
  /* ---------------------------------------------------------------- */

  /*
   * Integer scaling in DEVICE pixels, the same factor on both axes, as the
   * tab's dest_rect: one canvas pixel is one screen pixel, so the glyphs
   * and mosaics stay crisp and the scan lines land on real pixel rows.
   * Only when even 1x does not fit (a very narrow window at 1 dppx) is the
   * canvas shrunk by CSS, and then it is no longer crisp.
   */
  function fit() {
    var dpr = window.devicePixelRatio || 1;
    var availW = wrap.clientWidth;
    var availH = Math.max(180, window.innerHeight * 0.64);
    var maxS = E ? E._mtw_scale_max() : 8;
    var s = Math.floor(Math.min(availW * dpr / 320, availH * dpr / 250));
    if (s < 1) s = 1;
    if (s > maxS) s = maxS;
    if (s !== scale || canvas.width !== 320 * s) {
      scale = s;
      canvas.width = 320 * s;
      canvas.height = 250 * s;
    }
    var cssW = 320 * s / dpr, cssH = 250 * s / dpr;
    if (cssW > availW) { cssH = cssH * availW / cssW; cssW = availW; }
    canvas.style.width = cssW + 'px';
    canvas.style.height = cssH + 'px';
    queueDraw();
  }

  function queueDraw() {
    if (drawQueued) return;
    drawQueued = true;
    window.requestAnimationFrame(draw);
  }

  function draw() {
    drawQueued = false;
    if (!E) return;
    var r = 0, p = 0;
    if (mono) { r |= R_MONO; p |= P_MONO; }
    if ((tick >> 1) & 1) r |= R_FLASH;      /* flash: 1 s on, 1 s off (p84) */
    if (tick & 1) r |= R_CURSOR;            /* the cursor's phase (p99)     */
    if (scan) p |= P_SCANLINES;
    var ptr = E._mtw_frame(scale, r, p);
    if (!ptr) return;
    var w = 320 * scale, h = 250 * scale;
    var px = new Uint8ClampedArray(E.HEAPU8.buffer, ptr, w * h * 4);
    ctx.putImageData(new ImageData(px, w, h), 0, 0);
  }

  function updateCounts() {
    if (!E) return;
    var s = [];
    for (var i = 0; i < 11; i++) s.push(E._mtw_stat(i) >>> 0);
    countsEl.textContent =
      s[0] + ' bytes  ' + s[1] + ' chars  ' + s[2] + ' filtered  ' + s[3] + ' resync  ' +
      s[4] + ' error  ' + s[5] + ' row00  ' + s[6] + ' pro  ' + s[7] + ' unrep  ' + s[9] + ' bell' +
      (E._mtw_scroll_mode() ? '  [scroll]' : '') +
      (s[8] ? '  [8-bit bytes folded]' : '') +
      (nonByte ? '  [' + nonByte + ' non-byte chars]' : '');
  }

  function updateStart() {
    var show = state !== 'connected' && !(E && E._mtw_ever_drew());
    term.classList.toggle('show-start', show);
    term.classList.toggle('idle', state === 'idle');
    term.classList.toggle('connecting', state === 'connecting');
    if (state === 'connecting') {
      startGo.textContent = 'Connecting…';
      startHint.textContent = HOST;
    } else {
      var coarse = window.matchMedia && window.matchMedia('(pointer: coarse)').matches;
      startGo.textContent = coarse ? 'Tap to connect' : 'Click to connect';
      startHint.textContent = 'or press CONNEXION/FIN (F9)';
    }
  }

  /* ---------------------------------------------------------------- */
  /* The line                                                         */
  /* ---------------------------------------------------------------- */

  function feed(bytes) {
    if (!E || !bytes.length) return;
    var inb = E._mtw_inbuf(), max = E._mtw_inbuf_size();
    for (var o = 0; o < bytes.length; o += max) {
      var chunk = bytes.subarray(o, Math.min(o + max, bytes.length));
      E.HEAPU8.set(chunk, inb);
      E._mtw_feed(chunk.length);
    }
    /* The decoder's answers (PRO1 ENQROM, ESC 0x61) go up the line. */
    var n = E._mtw_reply_take();
    if (n > 0) {
      var rp = E._mtw_reply_ptr();
      sendBytes(E.HEAPU8.slice(rp, rp + n));
    }
  }

  function sendBytes(arr) {
    if (!ws || state !== 'connected') return false;
    var s = '';
    for (var i = 0; i < arr.length; i++) s += String.fromCharCode(arr[i] & 0x7F);
    ws.send(s);                 /* 7-bit: a TEXT frame carries it verbatim */
    rxSinceTx = false;
    return true;
  }

  function onData(data) {
    var bytes;
    if (typeof data === 'string') {
      bytes = new Uint8Array(data.length);
      for (var i = 0; i < data.length; i++) {
        var c = data.charCodeAt(i);
        if (c > 0xFF) { c = 0x1A; nonByte++; }   /* not a byte: SUB, the error symbol (p99) */
        bytes[i] = c;
      }
    } else {
      bytes = new Uint8Array(data);
    }
    if (!bytes.length) {
      /* The gateway's end-of-call marker (see GOODBYE_QUIET_MS). */
      if (state === 'connected') { goodbyeSeen = true; armGoodbyeClose(); }
      return;
    }
    rxQueue.push(bytes);
    rxQueued += bytes.length;
    rxSinceTx = true;
    lastRx = performance.now();
    if (goodbyeSeen) armGoodbyeClose();
  }

  function clearGoodbye() {
    goodbyeSeen = false;
    if (goodbyeTimer) { window.clearTimeout(goodbyeTimer); goodbyeTimer = 0; }
  }

  function armGoodbyeClose() {
    if (goodbyeTimer) window.clearTimeout(goodbyeTimer);
    goodbyeTimer = window.setTimeout(closeAfterGoodbye, GOODBYE_QUIET_MS);
  }

  /* The line has been quiet since the goodbye: hang up. What is still
   * queued is drawn first, then the session ends with the goodbye kept. */
  function closeAfterGoodbye() {
    goodbyeTimer = 0;
    var w = ws;
    if (!w || state !== 'connected') return;
    ws = null;
    w.onopen = w.onmessage = w.onclose = w.onerror = null;
    try { w.close(1000); } catch (e) { /* ignore */ }
    hostClosedAt = performance.now();
    if (!rxQueued) finishHostClose();
  }

  function clearRxQueue() {
    rxQueue = []; rxQueued = 0; paceCredit = 0; hostClosedAt = 0;
  }

  /* Take up to n queued bytes, in order. */
  function takeQueued(n) {
    var out = new Uint8Array(Math.min(n, rxQueued)), o = 0;
    while (o < out.length) {
      var head = rxQueue[0], k = Math.min(head.length, out.length - o);
      out.set(head.subarray(0, k), o);
      o += k;
      if (k === head.length) rxQueue.shift(); else rxQueue[0] = head.subarray(k);
    }
    rxQueued -= out.length;
    return out;
  }

  function pace() {
    var now = performance.now();
    var dt = paceLast ? now - paceLast : 1000 / PACE_HZ;
    paceLast = now;
    if (!rxQueued) {
      paceCredit = 0;
      if (hostClosedAt) finishHostClose();
      return;
    }
    if (document.hidden) {
      /* Nobody is watching: draw everything now. */
      paceCredit = rxQueued;
    } else {
      paceCredit += dt * PACE_BPS / 1000;
      if (rxQueued > PACE_BACKLOG) paceCredit += (rxQueued - PACE_BACKLOG) / 8;
    }
    var n = Math.floor(paceCredit);
    if (n < 1) return;
    paceCredit -= n;
    if (paceCredit > 8) paceCredit = 8;
    feed(takeQueued(n));
    queueDraw();
    updateCounts();
    if (!rxQueued && hostClosedAt) finishHostClose();
  }

  function clearLogoffTimer() {
    if (logoffTimer) { window.clearTimeout(logoffTimer); logoffTimer = 0; }
  }

  function connect() {
    if (!E) { connectWhenReady = true; return; }
    if (ws || state !== 'idle') return;
    clearLogoffTimer();
    clearRxQueue();
    clearGoodbye();
    E._mtw_reset();
    E._mtw_set_reply(1);
    E._mtw_set_status(70);                  /* 'F' until the line is up */
    nonByte = 0;
    state = 'connecting';
    opened = false;
    lastWasCnx = false;
    rxSinceTx = false;
    setStatus('Connecting to ' + HOST + '…');
    updateStart();
    updateCounts();
    queueDraw();

    var w;
    try {
      w = new WebSocket(WS_URL);
    } catch (e) {
      state = 'idle';
      E._mtw_set_reply(0);
      setStatus('Could not connect to ' + HOST);
      updateStart();
      return;
    }
    w.binaryType = 'arraybuffer';
    ws = w;
    w.onopen = function () {
      if (ws !== w) return;
      opened = true;
      state = 'connected';
      E._mtw_set_status(67);                /* 'C' (p32) */
      setStatus('Connected to ' + HOST);
      updateStart();
      queueDraw();
      focusScreen();
    };
    w.onmessage = function (e) {
      if (ws !== w) return;
      onData(e.data);
    };
    w.onerror = function () { /* onclose follows */ };
    w.onclose = function () {
      if (ws !== w) return;
      ws = null;
      if (!opened) {
        /* A connect that never succeeded gets no logoff (as the tab). */
        state = 'idle';
        E._mtw_set_reply(0);
        setStatus('Could not connect to ' + HOST + '. Try again in a moment.');
        updateStart();
        queueDraw();
        return;
      }
      /* Let what is still queued be drawn first, at the same pace: the
       * service's goodbye is often the last thing in the queue. */
      hostClosedAt = performance.now();
      if (!rxQueued) finishHostClose();
    };
  }

  function finishHostClose() {
    var at = hostClosedAt;
    hostClosedAt = 0;
    if (state !== 'connected') return;
    endSession(goodbyeSeen ? 'goodbye' : 'host', null, at);
  }

  /* The terminal drops the line itself. */
  function hangUp(why) {
    var w = ws;
    ws = null;
    if (w) {
      w.onopen = w.onmessage = w.onclose = w.onerror = null;
      try { w.close(1000); } catch (e) { /* ignore */ }
    }
    endSession('local', why);
  }

  function endSession(why, detail, closedAt) {
    var wasConnected = state === 'connected';
    /* Judged at the moment the host closed, as the tab judges it, not
     * when the paced drawing finished. */
    var goodbye = why === 'goodbye'
                  || (why === 'host' && rxSinceTx
                      && ((closedAt || performance.now()) - lastRx) <= GOODBYE_MS);
    if (why === 'local') clearRxQueue();
    clearGoodbye();
    state = 'idle';
    lastWasCnx = false;
    E._mtw_set_reply(0);
    E._mtw_set_status(70);                  /* 'F'; the screen stays (p88) */
    if (wasConnected) {
      if (!goodbye) drawLogoff();
      clearLogoffTimer();
      logoffTimer = window.setTimeout(startScreen, LOGOFF_MS);
    }
    setStatus(why === 'goodbye' ? 'Disconnected: the service said goodbye'
              : why === 'host' ? 'Host closed the connection'
              : 'Disconnected' + (detail ? ' (' + detail + ')' : ''));
    updateStart();
    updateCounts();
    queueDraw();
  }

  /* minitel_view_show_logoff: a Minitel page through the decoder. FF,
   * cursor off, two centred lines by US, row then column (p96). */
  function drawLogoff() {
    var l1 = 'Disconnected from', b = [0x0C, 0x14], i;
    b.push(0x1F, 0x40 + 11, 0x40 + 1 + ((40 - l1.length) >> 1));
    for (i = 0; i < l1.length; i++) b.push(l1.charCodeAt(i));
    b.push(0x1F, 0x40 + 13, 0x40 + 1 + ((40 - HOST.length) >> 1));
    for (i = 0; i < HOST.length; i++) b.push(HOST.charCodeAt(i));
    feed(new Uint8Array(b));
  }

  /* The power-on state again, with the start panel over it. */
  function startScreen() {
    logoffTimer = 0;
    if (state !== 'idle' || !E) return;
    E._mtw_reset();
    E._mtw_set_reply(0);
    lastWasCnx = false;
    setStatus('Back to the start screen');
    updateStart();
    updateCounts();
    queueDraw();
  }

  /* ---------------------------------------------------------------- */
  /* Keys                                                             */
  /* ---------------------------------------------------------------- */

  function pressSep(code) {
    if (goodbyeSeen) return;                /* the call is over; hanging up */
    if (code === 'I') {
      if (state === 'idle') { connect(); return; }
      if (state === 'connecting') return;
      if (lastWasCnx) { hangUp('CONNEXION/FIN twice'); return; }
    }
    if (state !== 'connected') return;
    sendBytes([0x13, code.charCodeAt(0)]);
    lastWasCnx = code === 'I';
    setStatus('Sent ' + SEP_NAME[code]);
  }

  function pressChar(ch) {
    if (state !== 'connected' || goodbyeSeen) return false;
    sendBytes([ch.charCodeAt(0)]);
    lastWasCnx = false;
    return true;
  }

  /* Show which on-screen key a PC key stands for. */
  function flashKey(sel) {
    var el = document.querySelector(sel);
    if (!el) return;
    el.classList.add('down');
    window.setTimeout(function () { el.classList.remove('down'); }, 120);
  }

  document.addEventListener('keydown', function (e) {
    var t = e.target;
    /* Leave links alone, and every browser shortcut with Ctrl, Alt or
     * Meta (AltGr, which some layouts need for @ or #, is let through). */
    if (t && t.closest && t.closest('a')) return;
    var altgr = e.getModifierState && e.getModifierState('AltGraph');
    if ((e.ctrlKey || e.altKey || e.metaKey) && !altgr) return;

    var sep = PC_KEYS[e.key];
    if (sep) {
      /* Tab must not move the focus, F1 must not open help, Backspace
       * must not go back, the arrows and Home must not scroll. */
      e.preventDefault();
      pressSep(sep);
      flashKey('[data-key="' + sep + '"]');
      return;
    }
    if (e.key && e.key.length === 1) {
      var c = e.key.charCodeAt(0);
      if (c < 0x20 || c > 0x7E) return;
      e.preventDefault();                   /* Space must not scroll */
      var ch = e.key.toUpperCase();         /* capitals (p110) */
      if (ch.length !== 1 || ch.charCodeAt(0) > 0x7E) ch = e.key;
      pressChar(ch);
      if (/^[0-9*#]$/.test(ch)) flashKey('[data-char="' + ch + '"]');
    }
  });

  /* The on-screen keys. The focus goes back to the screen afterwards, so
   * the PC keys go on working (as the tab does). */
  Array.prototype.forEach.call(document.querySelectorAll('.key'), function (b) {
    b.addEventListener('click', function () {
      if (b.dataset.key) pressSep(b.dataset.key);
      else if (b.dataset.char && pressChar(b.dataset.char)) setStatus('Sent ' + b.dataset.char);
      focusScreen();
    });
  });

  /* A click or tap on the screen connects while disconnected. */
  canvas.addEventListener('click', function () {
    if (state === 'idle') connect();
    focusScreen();
  });
  startBtn.addEventListener('click', function () {
    connect();
    focusScreen();
  });

  optScan.addEventListener('change', function () {
    scan = optScan.checked;
    prefSet('scan', scan);
    queueDraw();
    focusScreen();
  });
  optMono.addEventListener('change', function () {
    mono = optMono.checked;
    prefSet('mono', mono);
    queueDraw();
    focusScreen();
  });

  /* Leaving the page closes the socket at once. Chrome otherwise keeps
   * the page, socket and all, in its back/forward cache for 10 minutes,
   * holding a gateway slot (docs/2026-09-27_MINITEL_GATE_EMULATOR.md). */
  window.addEventListener('pagehide', function () {
    if (!ws) return;
    var w = ws;
    ws = null;
    w.onopen = w.onmessage = w.onclose = w.onerror = null;
    try { w.close(1000); } catch (e) { /* ignore */ }
    state = 'idle';
    clearRxQueue();
    clearGoodbye();
    clearLogoffTimer();
  });
  window.addEventListener('pageshow', function (e) {
    if (!e.persisted || !E) return;
    state = 'idle';
    E._mtw_set_status(70);
    startScreen();
    setStatus('Not connected');
  });

  window.addEventListener('resize', fit);
  if (window.matchMedia) {
    /* Re-fit when the device pixel ratio changes (zoom, another monitor). */
    (function watchDpr() {
      var mq = window.matchMedia('(resolution: ' + (window.devicePixelRatio || 1) + 'dppx)');
      var h = function () { mq.removeEventListener('change', h); fit(); watchDpr(); };
      if (mq.addEventListener) mq.addEventListener('change', h);
    })();
  }

  window.setInterval(pace, 1000 / PACE_HZ);

  window.setInterval(function () {
    if (document.hidden) return;
    tick++;
    queueDraw();
  }, BLINK_MS);

  /* ---------------------------------------------------------------- */
  /* Start                                                            */
  /* ---------------------------------------------------------------- */

  updateStart();
  if (typeof WebAssembly !== 'object' || typeof window.MinitelEngine !== 'function') {
    setStatus('This browser cannot run the terminal: WebAssembly is needed.');
    return;
  }
  window.MinitelEngine().then(function (m) {
    E = m;
    if (!E._mtw_init()) { setStatus('The terminal could not start.'); E = null; return; }
    fit();
    updateStart();
    updateCounts();
    focusScreen();
    if (connectWhenReady) { connectWhenReady = false; connect(); }
  }, function () {
    setStatus('The terminal could not start (engine.wasm did not load).');
  });
})();
