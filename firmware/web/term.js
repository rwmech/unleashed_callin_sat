// ===========================================================================
//  µnleashed gateway sat: the browser terminal
//
//  Copyright 2026 - Robert Mech
//  SPDX-License-Identifier: GPL-3.0-or-later
//
//  xterm.js plus a WebSocket, and the ten lines of glue that would have
//  been the attach addon. Written here rather than fetched because this
//  also has to tell the gateway the window size, so the gateway can tell
//  the board over telnet NAWS: a phone held upright then gets the board's
//  40-column layout and a phone on its side gets 80, and nobody chose
//  anything. No addon would have done that half.
//
//  THE WIRE, and it is worth stating because it is the one piece of
//  protocol this phase invents:
//
//    binary frames  the caller's bytes, both ways. Nothing is escaped and
//                   nothing is interpreted, so a caller can type any byte
//                   including the ones a control message would use.
//    text frames    control, gateway's way or ours. One line each:
//                     "s <cols> <rows>"   the window changed size (to us)
//                     "x <words>"         the line closed, and why (to you)
//
//  Splitting control from data by FRAME TYPE rather than by an escape is
//  what makes "a caller types the control sequence" impossible rather than
//  merely unlikely.
// ===========================================================================
(function () {
  'use strict';

  var q = new URLSearchParams(location.search);
  var board = parseInt(q.get('b'), 10);
  if (!(board >= 1 && board <= 5)) board = 1;

  var elGlass = document.getElementById('glass');
  var elState = document.getElementById('state');
  var elSize = document.getElementById('size');

  function state(text, cls) {
    elState.textContent = text;
    elState.className = cls || '';
  }

  var term = new window.Terminal({
    // CP437 art and PETSCII both want a cell that is exactly a cell, so a
    // monospace stack and nothing clever. No webfont: there is no internet
    // on this network to fetch one from.
    fontFamily: 'ui-monospace, SFMono-Regular, Menlo, Consolas, "DejaVu Sans Mono", monospace',
    fontSize: 14,
    // The board draws its own cursor behaviour; a blinking one on top of
    // that is two cursors.
    cursorBlink: false,
    // A BBS is a live line, not a log. The board redraws rather than
    // scrolls for its forms and refresh screens, so a deep scrollback is
    // memory spent on something nobody reads; a few screens is enough to
    // look back at a listing that paged past.
    scrollback: 500,
    allowProposedApi: false,
    // The board sends its own line endings and means them: a bare CR is a
    // cursor to column 1, which is how every in-place effect on the board
    // works (a rubbed-out password, a flashing refusal, the spinner).
    // Turning CR into CRLF here would scroll the screen on every one.
    convertEol: false,
    theme: {
      background: '#11131a',
      foreground: '#d7dae3',
      cursor: '#ffc14d'
    }
  });

  var fit = new window.FitAddon.FitAddon();
  term.loadAddon(fit);
  term.open(elGlass);

  var ws = null;
  var enc = new TextEncoder();
  var closed = false;

  // The gateway reads one WebSocket frame into a fixed buffer and closes
  // the line on anything bigger, because the IDF's server has already
  // taken the frame's header off the socket by the time the length is
  // known and cannot put it back. It does not reassemble fragments
  // either. So a paste is cut into pieces here, well under both limits,
  // and the order is preserved because one WebSocket carries frames in
  // order.
  var CHUNK = 512;

  function sendBytes(b) {
    if (!ws || ws.readyState !== 1) return;
    for (var i = 0; i < b.length; i += CHUNK) {
      ws.send(b.subarray(i, Math.min(i + CHUNK, b.length)));
    }
  }

  // The same bounds line.h keeps, so neither side has to guess what the
  // other calls a legal window. The gateway treats an out-of-range figure
  // as "use the default", so clamping here is what stops a very narrow
  // glass being silently told to the board as 80 columns and wrapping
  // every line.
  var COLS_MIN = 20, COLS_MAX = 240, ROWS_MIN = 5, ROWS_MAX = 240;

  function clamp(v, lo, hi) { return v < lo ? lo : (v > hi ? hi : v); }

  function showSize() {
    elSize.textContent = term.cols + 'x' + term.rows;
  }

  // The gateway is told the size, and it tells the board. Sent only when
  // it has actually changed: a resize event fires on every pixel of a
  // phone's address bar sliding away, and each one would otherwise be a
  // telnet subnegotiation and a board redraw.
  var sentCols = 0, sentRows = 0;
  function tellSize() {
    if (!ws || ws.readyState !== 1) return;
    var c = clamp(term.cols, COLS_MIN, COLS_MAX);
    var r = clamp(term.rows, ROWS_MIN, ROWS_MAX);
    if (c === sentCols && r === sentRows) return;
    sentCols = c;
    sentRows = r;
    ws.send('s ' + c + ' ' + r);
  }

  function refit() {
    try { fit.fit(); } catch (e) { /* the glass has no size yet */ }
    showSize();
    tellSize();
  }

  // Debounced, for the same reason: a phone rotating fires a stream of
  // these and only the last one is a real size.
  var refitTimer = 0;
  function refitSoon() {
    clearTimeout(refitTimer);
    refitTimer = setTimeout(refit, 120);
  }
  window.addEventListener('resize', refitSoon);
  if (window.visualViewport) {
    // iOS changes the visual viewport without a window resize when the
    // keyboard comes up, and a terminal drawn for the whole screen then
    // has its bottom rows under the keyboard.
    window.visualViewport.addEventListener('resize', refitSoon);
  }
  refit();

  var openCols = 0, openRows = 0;

  function connect() {
    openCols = clamp(term.cols, COLS_MIN, COLS_MAX);
    openRows = clamp(term.rows, ROWS_MIN, ROWS_MAX);
    var url = (location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host +
              '/ws?b=' + board + '&c=' + openCols + '&r=' + openRows;
    ws = new WebSocket(url);
    ws.binaryType = 'arraybuffer';

    ws.onopen = function () {
      state('connected', 'up');
      // NOT term.cols/term.rows: the size that reached the gateway is the
      // one in the URL, taken before the socket opened. A refit between
      // the two (a phone's address bar settling, the keyboard appearing -
      // the 120 ms debounce makes it easy) would otherwise be recorded as
      // already sent, and the board would draw at the stale width for the
      // whole call with nothing to correct it.
      sentCols = openCols;
      sentRows = openRows;
      tellSize();
      term.focus();
    };

    ws.onmessage = function (ev) {
      if (typeof ev.data === 'string') {
        // Control. Only one message comes this way today.
        if (ev.data.charAt(0) === 'x') {
          closed = true;
          state(ev.data.slice(2) || 'the line closed', 'gone');
        }
        return;
      }
      // The board's own bytes. xterm takes a Uint8Array and decodes UTF-8
      // itself, which is what the board expects: it re-encodes its CP437
      // art for a UTF-8 terminal, so the art arrives as the characters it
      // was drawn as rather than as bytes to guess at.
      term.write(new Uint8Array(ev.data));
    };

    ws.onclose = function () {
      if (!closed) state('the line closed', 'gone');
      term.write('\r\n');
      term.writeln('--> Disconnected. Tap "boards" below to call again.');
    };

    ws.onerror = function () {
      if (!closed) state('could not connect', 'gone');
    };
  }

  // Everything the caller types. xterm gives a string; the board wants
  // bytes, and UTF-8 is what it detected the terminal as.
  term.onData(function (d) {
    sendBytes(enc.encode(d));
  });

  // Pasting and anything else that arrives as a block goes the same way.
  term.onBinary(function (d) {
    var b = new Uint8Array(d.length);
    for (var i = 0; i < d.length; i++) b[i] = d.charCodeAt(i) & 0xFF;
    sendBytes(b);
  });

  // The bell. xterm 5.5 has no bell option at all (bellStyle went in 5.0),
  // only the event, so the sound is ours. It matters: the board rings for a
  // page, a broadcast, a caller arriving and a form refusing, and a board
  // that cannot ring loses something a caller notices.
  //
  // A browser will not make a sound before the caller has interacted with
  // the page, so the context is created on their first keystroke rather
  // than at load. Nothing is allocated on a caller who never types.
  var audio = null;
  function beep() {
    try {
      if (!audio) {
        var C = window.AudioContext || window.webkitAudioContext;
        if (!C) return;
        audio = new C();
      }
      if (audio.state === 'suspended') audio.resume();
      var o = audio.createOscillator();
      var g = audio.createGain();
      o.type = 'square';
      o.frequency.value = 880;
      g.gain.value = 0.05;
      o.connect(g);
      g.connect(audio.destination);
      o.start();
      o.stop(audio.currentTime + 0.08);
    } catch (e) { /* no audio here; a silent bell is not a fault */ }
  }
  term.onBell(beep);

  connect();
})();
