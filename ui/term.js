// SPDX-License-Identifier: MIT
//
// The Terminal: shells in the project, on the shelf.
//
// A shell runs on a pseudo-terminal of its own in the core
// (tools/terminal.hpp). What it prints is read here a piece at a time and
// played onto a screen the way a terminal plays it: the control sequences of
// the VT100 and of xterm, which is what every program that draws in a
// terminal writes, a prompt's colors and a pager's screen and an editor's
// alike. Keys pressed go back as the sequences a terminal sends for them.
// It is Crucible's own rather than a library's: what it needs is a screen, a
// parser and a keyboard, and the page has no packages.
//
// The screen's element is made once and kept outside what the views draw.
// Drawn through the page's patcher, a row of spans at a time thirty times a
// second, a terminal printing fast is what the window would spend its time
// on. The views draw a place for it, which the patcher leaves alone (its
// data-sig never changes), and after each draw it is put there.

// --- the screen -----------------------------------------------------------------------

/// Lines kept above the screen, to scroll back to.
const TERM_BACK = 5000;

/// xterm's sixteen colors, made to sit on Crucible's dark: eight, and the
/// eight bright ones.
const TERM_COLORS = [
  '#2E2E34', '#E5534B', '#57AB5A', '#C69026', '#539BF5', '#B083F0', '#39C5CF', '#C5C5CB',
  '#6E6E76', '#FF7B72', '#8DDB8C', '#E8C46A', '#82B8FF', '#DCBDFB', '#76E3EA', '#F4F4F6',
];

/// What a program asking the terminal its colors is told: the stylesheet's
/// --text and --panel, in the form xterm answers in.
const TERM_FG = 'rgb:ecec/ecec/ecec';
const TERM_BG = 'rgb:0e0e/0e0e/1010';

const termHex = (rgb) => `#${rgb.toString(16).padStart(6, '0')}`;

/// Color `n` of xterm's 256: the sixteen, a cube of six levels of each
/// primary, and a ramp of grays.
function termColor(n) {
  if (n < 16) return TERM_COLORS[n];
  if (n < 232) {
    const level = (v) => (v ? v * 40 + 55 : 0);
    const i = n - 16;
    return termHex((level(Math.floor(i / 36)) << 16) | (level(Math.floor(i / 6) % 6) << 8) | level(i % 6));
  }
  const gray = (n - 232) * 10 + 8;
  return termHex((gray << 16) | (gray << 8) | gray);
}

// How a character looks, as bits.
const T_BOLD = 1;
const T_DIM = 2;
const T_ITALIC = 4;
const T_UNDER = 8;
const T_BLINK = 16;
const T_INVERSE = 32;
const T_HIDDEN = 64;
const T_STRIKE = 128;

/// A color given as red, green and blue, rather than by its number.
const T_RGB = 0x1000000;

/// One look, shared by every cell that has it, so a run of them is told by
/// being the same object.
const termLooks = new Map();
function termLook(fg, bg, flags) {
  const key = `${fg},${bg},${flags}`;
  let look = termLooks.get(key);
  if (!look) {
    // A program painting a gradient makes thousands. Those on screen keep
    // theirs; only the table is started again.
    if (termLooks.size > 4096) termLooks.clear();
    look = { fg, bg, flags, css: null };
    termLooks.set(key, look);
  }
  return look;
}
const T_PLAIN = termLook(-1, -1, 0);

/// A look as a style attribute's value.
function termCss(look) {
  if (look.css !== null) return look.css;
  const { flags } = look;
  // Bold makes the first eight their bright twins, as xterm does.
  const fgNumber = (flags & T_BOLD) && look.fg >= 0 && look.fg < 8 ? look.fg + 8 : look.fg;
  const color = (n) => (n < 0 ? '' : n >= T_RGB ? termHex(n - T_RGB) : termColor(n));
  let fg = color(fgNumber);
  let bg = color(look.bg);
  if (flags & T_INVERSE) [fg, bg] = [bg || 'var(--term-bg)', fg || 'var(--term-fg)'];
  if (flags & T_HIDDEN) fg = bg || 'var(--term-bg)';
  const parts = [];
  if (fg) parts.push(`color:${fg}`);
  if (bg) parts.push(`background:${bg}`);
  if (flags & T_BOLD) parts.push('font-weight:700');
  if (flags & T_DIM) parts.push('opacity:.6');
  if (flags & T_ITALIC) parts.push('font-style:italic');
  const lines = [flags & T_UNDER ? 'underline' : '', flags & T_STRIKE ? 'line-through' : ''].filter(Boolean);
  if (lines.length) parts.push(`text-decoration:${lines.join(' ')}`);
  look.css = parts.join(';');
  return look.css;
}

/// What takes no room of its own: marks that go on the character before,
/// and the joiners and selectors between characters.
const T_ZERO = /[\p{Mn}\p{Me}\p{Cf}]/u;

/// How many cells `ch` (code point `cp`) takes: 0, 1, or 2 for the wide ones
/// of East Asian scripts and the emoji drawn as pictures.
function termWidth(cp, ch) {
  if (cp < 0x300) return 1;
  if (T_ZERO.test(ch)) return 0;
  if (cp >= 0x1100 && (cp <= 0x115f || cp === 0x2329 || cp === 0x232a
      || (cp >= 0x2e80 && cp <= 0xa4cf && cp !== 0x303f) || (cp >= 0xac00 && cp <= 0xd7a3)
      || (cp >= 0xf900 && cp <= 0xfaff) || (cp >= 0xfe30 && cp <= 0xfe4f) || (cp >= 0xff00 && cp <= 0xff60)
      || (cp >= 0xffe0 && cp <= 0xffe6) || (cp >= 0x1f300 && cp <= 0x1f64f) || (cp >= 0x1f900 && cp <= 0x1f9ff)
      || (cp >= 0x1fa70 && cp <= 0x1faff) || (cp >= 0x20000 && cp <= 0x3fffd))) return 2;
  return 1;
}

/// The VT100's line-drawing set, which a program switches to for the edges
/// of boxes: the letter it writes, and what is drawn.
const T_LINES = {
  '`': '\u25c6', a: '\u2592', b: '\u2409', c: '\u240c', d: '\u240d', e: '\u240a', f: '\u00b0', g: '\u00b1',
  h: '\u2424', i: '\u240b', j: '\u2518', k: '\u2510', l: '\u250c', m: '\u2514', n: '\u253c', o: '\u23ba',
  p: '\u23bb', q: '\u2500', r: '\u23bc', s: '\u23bd', t: '\u251c', u: '\u2524', v: '\u2534', w: '\u252c',
  x: '\u2502', y: '\u2264', z: '\u2265', '{': '\u03c0', '|': '\u2260', '}': '\u00a3', '~': '\u00b7',
};

// Where the parser is in what it reads.
const T_GROUND = 0;
const T_ESC = 1;
const T_CSI = 2;
const T_OSC = 3;
const T_OSC_ESC = 4;
const T_STRING = 5;
const T_STRING_ESC = 6;
const T_CHARSET = 7;
const T_HASH = 8;

/// A screen `width` by `height`, and what plays a program's output onto it.
/// `reply` is handed what the terminal says back when asked: where its
/// cursor is, what kind of terminal it is, its colors.
function termScreen(width, height, reply) {
  let cols = width;
  let rows = height;
  let x = 0;
  let y = 0;
  let wrapNext = false;   // the last column was written: the next character starts a line
  let fg = -1;
  let bg = -1;
  let flags = 0;
  let pen = T_PLAIN;
  let top = 0;
  let bottom = rows - 1;
  let tabs = [];
  let charsets = ['B', 'B', 'B', 'B'];
  let shifted = 0;
  let last = ' ';
  const saved = [null, null];   // the cursor saved on each screen
  const modes = {
    appCursor: false, appKeypad: false, wrap: true, origin: false, insert: false, newline: false,
    cursor: true, paste: false, mouse: 0, sgrMouse: false, focus: false, alt: false,
  };
  let title = '';
  const dirty = new Set();
  let whole = true;
  let backed = [];       // lines gone off the top since the last look
  let cleared = false;   // the lines above were wiped

  let parsing = T_GROUND;
  let prefix = '';
  let params = '';
  let inter = '';
  let osc = '';
  let target = 0;

  /// What erasing leaves: the background being written with, and no more.
  const erased = () => (bg === -1 ? T_PLAIN : termLook(-1, bg, 0));
  const blankLine = () => ({ ch: new Array(cols).fill(' '), at: new Array(cols).fill(erased()), wrapped: false });
  const blankScreen = () => Array.from({ length: rows }, blankLine);
  let main = blankScreen();
  let alt = blankScreen();
  let lines = main;

  function setTabs() {
    tabs = [];
    for (let i = 0; i < cols; i += 8) tabs[i] = true;
  }
  setTabs();

  const touch = (row) => dirty.add(row);
  const touchAll = () => { whole = true; };

  function moveTo(cx, cy) {
    x = Math.max(0, Math.min(cols - 1, cx));
    y = modes.origin ? Math.max(top, Math.min(bottom, cy + top)) : Math.max(0, Math.min(rows - 1, cy));
    wrapNext = false;
  }

  function scrollUp(n) {
    for (let k = 0; k < Math.min(n, rows); k++) {
      const gone = lines.splice(top, 1)[0];
      lines.splice(bottom, 0, blankLine());
      // Off the top of the whole screen, the line goes above it to be
      // scrolled back to. Not from the other screen, which is a program's
      // own, and not from a part of the screen a program set to scroll.
      if (lines === main && top === 0 && bottom === rows - 1) backed.push(gone);
    }
    for (let r = top; r <= bottom; r++) touch(r);
  }

  function scrollDown(n) {
    for (let k = 0; k < Math.min(n, rows); k++) {
      lines.splice(bottom, 1);
      lines.splice(top, 0, blankLine());
    }
    for (let r = top; r <= bottom; r++) touch(r);
  }

  function index() {
    if (y === bottom) scrollUp(1);
    else if (y < rows - 1) y++;
    wrapNext = false;
  }

  function reverseIndex() {
    if (y === top) scrollDown(1);
    else if (y > 0) y--;
    wrapNext = false;
  }

  /// A wide character half written over is a blank now.
  function unwiden(line, i) {
    if (i > 0 && line.ch[i] === '') line.ch[i - 1] = ' ';
    if (i + 1 < cols && line.ch[i + 1] === '') line.ch[i + 1] = ' ';
  }

  function print(ch, cp) {
    if (charsets[shifted] === '0' && cp >= 0x60 && cp <= 0x7e) ch = T_LINES[ch] || ch;
    const w = termWidth(cp, ch);
    if (w === 0) {
      // A mark goes on the character before it.
      let px = wrapNext ? x : x - 1;
      if (px < 0) return;
      const line = lines[y];
      if (line.ch[px] === '' && px > 0) px--;
      line.ch[px] += ch;
      touch(y);
      return;
    }
    if (wrapNext && modes.wrap) {
      lines[y].wrapped = true;
      x = 0;
      index();
    }
    wrapNext = false;
    if (w === 2 && x === cols - 1) {
      if (!modes.wrap) return;
      lines[y].ch[x] = ' ';
      lines[y].wrapped = true;
      touch(y);
      x = 0;
      index();
    }
    const line = lines[y];
    if (modes.insert) {
      line.ch.splice(x, 0, ...new Array(w).fill(' '));
      line.at.splice(x, 0, ...new Array(w).fill(pen));
      line.ch.length = cols;
      line.at.length = cols;
    }
    unwiden(line, x);
    if (w === 2) unwiden(line, x + 1);
    line.ch[x] = ch;
    line.at[x] = pen;
    if (w === 2) {
      line.ch[x + 1] = '';
      line.at[x + 1] = pen;
    }
    touch(y);
    last = ch;
    x += w;
    if (x >= cols) {
      x = cols - 1;
      wrapNext = modes.wrap;
    }
  }

  function tabForward() {
    let at = x + 1;
    while (at < cols - 1 && !tabs[at]) at++;
    x = Math.min(cols - 1, at);
    wrapNext = false;
  }

  function tabBack() {
    let at = x - 1;
    while (at > 0 && !tabs[at]) at--;
    x = Math.max(0, at);
    wrapNext = false;
  }

  function control(code) {
    switch (code) {
      case 8:   // backspace
        wrapNext = false;
        if (x > 0) x--;
        break;
      case 9: tabForward(); break;
      case 10: case 11: case 12:
        index();
        if (modes.newline) x = 0;
        break;
      case 13: x = 0; wrapNext = false; break;
      case 14: shifted = 1; break;
      case 15: shifted = 0; break;
      case 27: parsing = T_ESC; break;
      default: break;   // the bell, and the rest, say nothing on screen
    }
  }

  function saveCursor() {
    saved[lines === alt ? 1 : 0] = { x, y, fg, bg, flags, origin: modes.origin, charsets: charsets.slice(), shifted, wrapNext };
  }

  function restoreCursor() {
    const was = saved[lines === alt ? 1 : 0];
    if (!was) {
      moveTo(0, 0);
      return;
    }
    ({ fg, bg, flags, shifted, wrapNext } = was);
    charsets = was.charsets.slice();
    modes.origin = was.origin;
    pen = termLook(fg, bg, flags);
    x = Math.min(cols - 1, was.x);
    y = Math.min(rows - 1, was.y);
  }

  function useAlt(on, clear) {
    if (on && lines !== alt) {
      if (clear) alt = blankScreen();
      lines = alt;
    } else if (!on && lines === alt) {
      lines = main;
    }
    modes.alt = lines === alt;
    touchAll();
  }

  function softReset() {
    Object.assign(modes, { appCursor: false, appKeypad: false, wrap: true, origin: false, insert: false, cursor: true });
    top = 0;
    bottom = rows - 1;
    fg = bg = -1;
    flags = 0;
    pen = T_PLAIN;
    charsets = ['B', 'B', 'B', 'B'];
    shifted = 0;
    saved[0] = saved[1] = null;
  }

  function fullReset() {
    softReset();
    Object.assign(modes, { newline: false, paste: false, mouse: 0, sgrMouse: false, focus: false, alt: false });
    main = blankScreen();
    alt = blankScreen();
    lines = main;
    x = y = 0;
    wrapNext = false;
    title = '';
    setTabs();
    cleared = true;
    touchAll();
  }

  function eraseLine(mode) {
    const line = lines[y];
    const from = mode === 0 ? x : 0;
    const to = mode === 1 ? x : cols - 1;
    const look = erased();
    for (let i = from; i <= to; i++) {
      line.ch[i] = ' ';
      line.at[i] = look;
    }
    if (from > 0 && line.ch[from] === '') line.ch[from - 1] = ' ';
    if (to + 1 < cols && line.ch[to + 1] === '') line.ch[to + 1] = ' ';
    if (mode !== 1) line.wrapped = false;
    touch(y);
  }

  function eraseDisplay(mode) {
    if (mode === 3) {
      cleared = true;
      return;
    }
    if (mode === 0 || mode === 1) {
      eraseLine(mode);
      const [from, to] = mode === 0 ? [y + 1, rows - 1] : [0, y - 1];
      for (let r = from; r <= to; r++) {
        lines[r] = blankLine();
        touch(r);
      }
      return;
    }
    for (let r = 0; r < rows; r++) lines[r] = blankLine();
    touchAll();
  }

  function insertLines(n) {
    if (y < top || y > bottom) return;
    for (let k = 0; k < Math.min(n, bottom - y + 1); k++) {
      lines.splice(bottom, 1);
      lines.splice(y, 0, blankLine());
    }
    for (let r = y; r <= bottom; r++) touch(r);
    x = 0;
    wrapNext = false;
  }

  function deleteLines(n) {
    if (y < top || y > bottom) return;
    for (let k = 0; k < Math.min(n, bottom - y + 1); k++) {
      lines.splice(y, 1);
      lines.splice(bottom, 0, blankLine());
    }
    for (let r = y; r <= bottom; r++) touch(r);
    x = 0;
    wrapNext = false;
  }

  function insertChars(n) {
    const line = lines[y];
    const count = Math.min(n, cols - x);
    line.ch.splice(x, 0, ...new Array(count).fill(' '));
    line.at.splice(x, 0, ...new Array(count).fill(erased()));
    line.ch.length = cols;
    line.at.length = cols;
    touch(y);
  }

  function deleteChars(n) {
    const line = lines[y];
    const count = Math.min(n, cols - x);
    line.ch.splice(x, count);
    line.at.splice(x, count);
    for (let k = 0; k < count; k++) {
      line.ch.push(' ');
      line.at.push(erased());
    }
    touch(y);
  }

  function eraseChars(n) {
    const line = lines[y];
    const look = erased();
    for (let i = x; i < Math.min(cols, x + n); i++) {
      line.ch[i] = ' ';
      line.at[i] = look;
    }
    touch(y);
  }

  /// Select Graphic Rendition: how what is written next looks.
  function sgr(text) {
    const list = text === '' ? ['0'] : text.split(';');
    for (let i = 0; i < list.length; i++) {
      const sub = list[i].split(':');
      const code = parseInt(sub[0], 10) || 0;
      if (code === 38 || code === 48 || code === 58) {
        // A color by number or by red, green and blue, in the form with
        // semicolons and the one with colons.
        let color = null;
        const rgbOf = (r, g, b) => T_RGB + (((r & 255) << 16) | ((g & 255) << 8) | (b & 255));
        if (sub.length > 1) {
          if (sub[1] === '5') color = parseInt(sub[2], 10);
          else if (sub[1] === '2') color = sub.length >= 6 ? rgbOf(sub[3], sub[4], sub[5]) : rgbOf(sub[2], sub[3], sub[4]);
        } else if (list[i + 1] === '5') {
          color = parseInt(list[i + 2], 10);
          i += 2;
        } else if (list[i + 1] === '2') {
          color = rgbOf(list[i + 2], list[i + 3], list[i + 4]);
          i += 4;
        }
        if (color !== null && !Number.isNaN(color)) {
          if (code === 38) fg = color;
          else if (code === 48) bg = color;
        }
        continue;
      }
      switch (code) {
        case 0: fg = bg = -1; flags = 0; break;
        case 1: flags |= T_BOLD; break;
        case 2: flags |= T_DIM; break;
        case 3: flags |= T_ITALIC; break;
        case 4: flags = sub[1] === '0' ? flags & ~T_UNDER : flags | T_UNDER; break;
        case 5: case 6: flags |= T_BLINK; break;
        case 7: flags |= T_INVERSE; break;
        case 8: flags |= T_HIDDEN; break;
        case 9: flags |= T_STRIKE; break;
        case 21: flags |= T_UNDER; break;
        case 22: flags &= ~(T_BOLD | T_DIM); break;
        case 23: flags &= ~T_ITALIC; break;
        case 24: flags &= ~T_UNDER; break;
        case 25: flags &= ~T_BLINK; break;
        case 27: flags &= ~T_INVERSE; break;
        case 28: flags &= ~T_HIDDEN; break;
        case 29: flags &= ~T_STRIKE; break;
        case 39: fg = -1; break;
        case 49: bg = -1; break;
        default:
          if (code >= 30 && code <= 37) fg = code - 30;
          else if (code >= 40 && code <= 47) bg = code - 40;
          else if (code >= 90 && code <= 97) fg = code - 90 + 8;
          else if (code >= 100 && code <= 107) bg = code - 100 + 8;
          break;
      }
    }
    pen = termLook(fg, bg, flags);
  }

  /// The modes set and reset with CSI ? ... h and l.
  function privateModes(list, on) {
    for (const one of list) {
      const n = parseInt(one, 10);
      switch (n) {
        case 1: modes.appCursor = on; break;
        case 6: modes.origin = on; moveTo(0, 0); break;
        case 7: modes.wrap = on; break;
        case 25: modes.cursor = on; touch(y); break;
        case 47: case 1047: useAlt(on, on); break;
        case 1048: if (on) saveCursor(); else restoreCursor(); break;
        case 1049:
          if (on) {
            saveCursor();
            useAlt(true, true);
          } else {
            useAlt(false);
            restoreCursor();
          }
          break;
        case 9: case 1000: case 1002: case 1003: modes.mouse = on ? n : 0; break;
        case 1004: modes.focus = on; break;
        case 1006: modes.sgrMouse = on; break;
        case 2004: modes.paste = on; break;
        default: break;
      }
    }
  }

  function csi(final) {
    const list = params.split(';');
    const count = (i) => Math.max(1, parseInt(list[i], 10) || 1);
    const value = (i, otherwise) => {
      const got = parseInt(list[i], 10);
      return Number.isNaN(got) ? otherwise : got;
    };
    if (prefix === '?') {
      if (final === 'h' || final === 'l') privateModes(list, final === 'h');
      else if (final === 'J') eraseDisplay(value(0, 0));
      else if (final === 'K') eraseLine(value(0, 0));
      return;
    }
    if (prefix === '>') {
      if (final === 'c') reply('\x1b[>0;276;0c');
      return;
    }
    if (prefix) return;
    if (inter === '!' && final === 'p') {
      softReset();
      return;
    }
    if (inter) return;   // the cursor's shape, and requests nothing here answers
    switch (final) {
      case '@': insertChars(count(0)); break;
      case 'A': y = Math.max(y >= top ? top : 0, y - count(0)); wrapNext = false; break;
      case 'B': y = Math.min(y <= bottom ? bottom : rows - 1, y + count(0)); wrapNext = false; break;
      case 'C': case 'a': x = Math.min(cols - 1, x + count(0)); wrapNext = false; break;
      case 'D': x = Math.max(0, x - count(0)); wrapNext = false; break;
      case 'E': x = 0; y = Math.min(y <= bottom ? bottom : rows - 1, y + count(0)); wrapNext = false; break;
      case 'F': x = 0; y = Math.max(y >= top ? top : 0, y - count(0)); wrapNext = false; break;
      case 'G': case '`': x = Math.min(cols - 1, count(0) - 1); wrapNext = false; break;
      case 'H': case 'f': moveTo(count(1) - 1, count(0) - 1); break;
      case 'I': for (let k = count(0); k > 0; k--) tabForward(); break;
      case 'Z': for (let k = count(0); k > 0; k--) tabBack(); break;
      case 'J': eraseDisplay(value(0, 0)); break;
      case 'K': eraseLine(value(0, 0)); break;
      case 'L': insertLines(count(0)); break;
      case 'M': deleteLines(count(0)); break;
      case 'P': deleteChars(count(0)); break;
      case 'S': scrollUp(count(0)); break;
      case 'T': if (list.length <= 1) scrollDown(count(0)); break;
      case 'X': eraseChars(count(0)); break;
      case 'b': for (let k = Math.min(count(0), cols * rows); k > 0; k--) print(last, last.codePointAt(0)); break;
      case 'c': if (value(0, 0) === 0) reply('\x1b[?62;22c'); break;
      case 'd': moveTo(x, count(0) - 1); break;
      case 'e': y = Math.min(rows - 1, y + count(0)); wrapNext = false; break;
      case 'g':
        if (value(0, 0) === 0) tabs[x] = false;
        else if (value(0, 0) === 3) tabs = [];
        break;
      case 'h': case 'l':
        for (const one of list) {
          if (one === '4') modes.insert = final === 'h';
          if (one === '20') modes.newline = final === 'h';
        }
        break;
      case 'm': sgr(params); break;
      case 'n':
        if (value(0, 0) === 5) reply('\x1b[0n');
        else if (value(0, 0) === 6) reply(`\x1b[${(modes.origin ? y - top : y) + 1};${x + 1}R`);
        break;
      case 'r': {
        const from = count(0) - 1;
        const to = Math.min(rows, value(1, rows) || rows) - 1;
        if (from < to) {
          top = from;
          bottom = to;
          moveTo(0, 0);
        }
        break;
      }
      case 's': if (params === '') saveCursor(); break;
      case 'u': restoreCursor(); break;
      case 't': if (value(0, 0) === 18) reply(`\x1b[8;${rows};${cols}t`); break;
      default: break;
    }
  }

  function escaped(c) {
    parsing = T_GROUND;
    switch (c) {
      case '[': parsing = T_CSI; prefix = params = inter = ''; break;
      case ']': parsing = T_OSC; osc = ''; break;
      case 'P': case 'X': case '^': case '_': parsing = T_STRING; break;
      case '(': case ')': case '*': case '+': parsing = T_CHARSET; target = '()*+'.indexOf(c); break;
      case '#': parsing = T_HASH; break;
      case '7': saveCursor(); break;
      case '8': restoreCursor(); break;
      case 'D': index(); break;
      case 'E': x = 0; index(); break;
      case 'M': reverseIndex(); break;
      case 'H': tabs[x] = true; break;
      case 'c': fullReset(); break;
      case '=': modes.appKeypad = true; break;
      case '>': modes.appKeypad = false; break;
      default: break;
    }
  }

  /// An operating system command: a title, a question about colors. Where
  /// the shell is, links and the clipboard are let pass.
  function command() {
    const at = osc.indexOf(';');
    const code = at < 0 ? osc : osc.slice(0, at);
    const text = at < 0 ? '' : osc.slice(at + 1);
    if (code === '0' || code === '2') title = text.slice(0, 200);
    else if ((code === '10' || code === '11') && text === '?') reply(`\x1b]${code};${code === '10' ? TERM_FG : TERM_BG}\x1b\\`);
  }

  /// Play `text`, as a program printed it, onto the screen.
  function feed(text) {
    for (let i = 0; i < text.length; i++) {
      let code = text.charCodeAt(i);
      if (parsing === T_GROUND) {
        if (code < 0x20 || code === 0x7f) {
          control(code);
          continue;
        }
        let ch = text[i];
        if (code >= 0xd800 && code <= 0xdbff && i + 1 < text.length) {
          ch = text.slice(i, i + 2);
          code = ch.codePointAt(0);
          i++;
        }
        print(ch, code);
        continue;
      }
      const c = text[i];
      // Inside a sequence, a control still does what it does; ESC starts
      // another and CAN or SUB ends this one. A string's controls are its own.
      if (code < 0x20 && parsing !== T_OSC && parsing !== T_STRING) {
        if (code === 0x18 || code === 0x1a) parsing = T_GROUND;
        else if (code === 0x1b) parsing = T_ESC;
        else control(code);
        continue;
      }
      switch (parsing) {
        case T_ESC:
          escaped(c);
          break;
        case T_CSI:
          if (code >= 0x30 && code <= 0x3f) {
            if (code >= 0x3c && params === '' && prefix === '') prefix = c;
            else if (code <= 0x3b) params += c;
          } else if (code >= 0x20 && code <= 0x2f) {
            inter += c;
          } else if (code >= 0x40 && code <= 0x7e) {
            parsing = T_GROUND;
            csi(c);
          } else {
            parsing = T_GROUND;
          }
          if (params.length > 256) parsing = T_GROUND;
          break;
        case T_OSC:
          if (code === 7) {
            parsing = T_GROUND;
            command();
          } else if (code === 0x1b) {
            parsing = T_OSC_ESC;
          } else if (code >= 0x20 && osc.length < 4096) {
            osc += c;
          }
          break;
        case T_OSC_ESC:
          command();
          parsing = T_GROUND;
          if (c !== '\\') {
            parsing = T_ESC;
            i--;
          }
          break;
        case T_STRING:
          if (code === 0x1b) parsing = T_STRING_ESC;
          else if (code === 7 || code === 0x18 || code === 0x1a) parsing = T_GROUND;
          break;
        case T_STRING_ESC:
          parsing = T_GROUND;
          if (c !== '\\') {
            parsing = T_ESC;
            i--;
          }
          break;
        case T_CHARSET:
          if (code < 0x30) break;   // an intermediate of a longer name
          charsets[target] = c;
          parsing = T_GROUND;
          break;
        case T_HASH:
          // The screen alignment test fills it with E.
          if (c === '8') {
            for (const line of lines) {
              line.ch.fill('E');
              line.at.fill(T_PLAIN);
            }
            touchAll();
          }
          parsing = T_GROUND;
          break;
        default:
          parsing = T_GROUND;
          break;
      }
    }
  }

  /// A new size. Lines are not wrapped again: a line wider than the screen
  /// is cut, and the shell draws its prompt again at the new width.
  function resize(w, h) {
    if (w === cols && h === rows) return;
    const fit = (screen, keep) => {
      // Shorter: what is above the cursor goes up into the lines kept
      // above, so the cursor stays on the screen.
      if (keep && y >= h) {
        const off = y - h + 1;
        backed.push(...screen.splice(0, off));
      }
      while (screen.length > h) screen.pop();
      for (const line of screen) {
        if (line.ch.length > w) {
          line.ch.length = w;
          line.at.length = w;
          if (line.ch[w - 1] === '') line.ch[w - 1] = ' ';
        }
        while (line.ch.length < w) {
          line.ch.push(' ');
          line.at.push(T_PLAIN);
        }
      }
      while (screen.length < h) screen.push({ ch: new Array(w).fill(' '), at: new Array(w).fill(T_PLAIN), wrapped: false });
    };
    const shift = lines === main && y >= h ? y - h + 1 : 0;
    fit(main, lines === main);
    fit(alt, false);
    cols = w;
    rows = h;
    y = Math.max(0, Math.min(rows - 1, y - shift));
    x = Math.min(cols - 1, x);
    wrapNext = false;
    top = 0;
    bottom = rows - 1;
    setTabs();
    touchAll();
  }

  return {
    feed,
    resize,
    reset: fullReset,
    modes,
    get cols() { return cols; },
    get rows() { return rows; },
    get title() { return title; },
    cursor: () => ({ x, y, shown: modes.cursor }),
    line: (row) => lines[row],
    /// What changed since the last look: every row, or these.
    changes() {
      const out = { whole, rows: Array.from(dirty) };
      whole = false;
      dirty.clear();
      return out;
    },
    /// The lines gone off the top since the last look, and whether the
    /// lines kept above were wiped.
    gone() {
      const out = { lines: backed, cleared };
      backed = [];
      cleared = false;
      return out;
    },
    /// What is on the screen, as text, a line each: for the tests.
    text: () => lines.map((line) => line.ch.join('').replace(/\s+$/, '')).join('\n'),
  };
}

/// One line as markup: a span for each run of one look, and the cursor's
/// cell in a span of its own when `cursor` is its column.
function termLineHtml(line, cursor) {
  let out = '';
  let run = '';
  let look = null;
  const flush = () => {
    if (run) out += look === T_PLAIN ? escape(run) : `<span style="${termCss(look)}">${escape(run)}</span>`;
    run = '';
  };
  // Blanks at the end are left out, past the cursor.
  let end = line.ch.length;
  while (end > 0 && end - 1 > cursor && line.ch[end - 1] === ' ' && line.at[end - 1] === T_PLAIN) end--;
  for (let i = 0; i < end; i++) {
    const ch = line.ch[i];
    if (ch === '') continue;   // the right half of a wide character
    if (i === cursor) {
      flush();
      look = null;
      const css = termCss(line.at[i]);
      out += `<span class="term-cursor"${css ? ` style="${css}"` : ''}>${escape(ch)}</span>`;
      continue;
    }
    if (line.at[i] !== look) {
      flush();
      look = line.at[i];
    }
    run += ch;
  }
  flush();
  return out;
}

// --- the keyboard ---------------------------------------------------------------------

/// What a key pressed sends, as a terminal sends it, or null when it is text
/// the input event brings instead. `mac` keeps Option for the characters it
/// types, as a Mac's terminal does.
function termKey(event, modes, mac) {
  const { key } = event;
  const ctrl = event.ctrlKey;
  const alt = event.altKey;
  const shift = event.shiftKey;
  if (event.metaKey) return null;
  // AltGr types characters, and says it is Ctrl and Alt doing it.
  if (ctrl && alt && key.length === 1 && event.getModifierState && event.getModifierState('AltGraph')) return null;
  const mod = 1 + (shift ? 1 : 0) + (alt ? 2 : 0) + (ctrl ? 4 : 0);
  const cursor = (final) => (mod > 1 ? `\x1b[1;${mod}${final}` : modes.appCursor ? `\x1bO${final}` : `\x1b[${final}`);
  const tilde = (n) => (mod > 1 ? `\x1b[${n};${mod}~` : `\x1b[${n}~`);
  const fn = (final) => (mod > 1 ? `\x1b[1;${mod}${final}` : `\x1bO${final}`);
  const meta = alt && !mac ? '\x1b' : '';
  switch (key) {
    case 'Enter': return `${meta}\r`;
    case 'Backspace': return meta + (ctrl ? '\x08' : '\x7f');
    case 'Tab': return shift ? '\x1b[Z' : '\t';
    case 'Escape': return '\x1b';
    case 'ArrowUp': return cursor('A');
    case 'ArrowDown': return cursor('B');
    case 'ArrowRight': return cursor('C');
    case 'ArrowLeft': return cursor('D');
    case 'Home': return cursor('H');
    case 'End': return cursor('F');
    case 'Insert': return tilde(2);
    case 'Delete': return tilde(3);
    case 'PageUp': return tilde(5);
    case 'PageDown': return tilde(6);
    case 'F1': return fn('P');
    case 'F2': return fn('Q');
    case 'F3': return fn('R');
    case 'F4': return fn('S');
    case 'F5': return tilde(15);
    case 'F6': return tilde(17);
    case 'F7': return tilde(18);
    case 'F8': return tilde(19);
    case 'F9': return tilde(20);
    case 'F10': return tilde(21);
    case 'F11': return tilde(23);
    case 'F12': return tilde(24);
    default: break;
  }
  if (key.length !== 1) return null;
  if (ctrl && !alt) {
    const c = key.toLowerCase();
    if (c >= 'a' && c <= 'z') return String.fromCharCode(c.charCodeAt(0) - 96);
    // Not the digits: Ctrl and a number are the window's, for its tabs.
    const named = { '@': '\x00', ' ': '\x00', '[': '\x1b', '\\': '\x1c', ']': '\x1d', '^': '\x1e', _: '\x1f',
                    '-': '\x1f', '/': '\x1f', '?': '\x7f' };
    return named[c] !== undefined ? named[c] : null;
  }
  if (ctrl && alt) {
    const c = key.toLowerCase();
    if (c >= 'a' && c <= 'z') return `\x1b${String.fromCharCode(c.charCodeAt(0) - 96)}`;
  }
  if (alt && !ctrl && !mac) return `\x1b${key}`;
  return null;
}

// --- the terminals --------------------------------------------------------------------
//
// Each terminal open: what it is in the core, the screen it plays onto, the
// element that shows it once it has been shown, and where its reading and
// writing are. The page's state holds only what the views draw.

state.term = { list: [], active: 0, error: '', synced: false };
const terms = new Map();

/// The name a terminal's tab has: what the program in it says it is, or the
/// shell's.
const termName = (one) => one.title || one.shell || 'Terminal';

/// Start a terminal, the size the last one shown was.
async function termOpen() {
  const size = termSize || { cols: 100, rows: 30 };
  const got = await call('term.open', size);
  termAdopt({ id: got.id, shell: got.shell, cwd: got.cwd, running: true }, size, 0);
  state.term.active = got.id;
  state.term.error = '';
  render();
}

/// Keep `info`, a terminal the core has, and start reading it from `since`.
function termAdopt(info, size, since) {
  const t = { id: info.id, next: since, out: '', writing: false, wait: 16, timer: 0, poke: null, view: null };
  t.screen = termScreen(size.cols, size.rows, (data) => termSend(t, data));
  terms.set(t.id, t);
  state.term.list.push({ id: info.id, shell: info.shell || '', cwd: info.cwd || '', title: '', running: info.running !== false });
  termPump(t);
  return t;
}

const termInfo = (id) => state.term.list.find((one) => one.id === id);

/// Read what it prints, for as long as it is open: quickly while it prints,
/// and less often the longer it has said nothing.
async function termPump(t) {
  while (terms.get(t.id) === t) {
    let got;
    try {
      got = await call('term.read', { id: t.id, since: t.next });
    } catch (error) {
      termEnded(t, null);
      return;
    }
    if (terms.get(t.id) !== t) return;
    if (got.data) {
      t.screen.feed(got.data);
      termDraw(t);
      termSettle();
    }
    t.next = got.next;
    if (!got.running && !got.data) {
      termEnded(t, got.status);
      return;
    }
    const shown = t.view && t.view.root.isConnected;
    t.wait = got.data ? 16 : Math.min(t.wait * 2, shown ? 200 : 1000);
    await new Promise((resolve) => {
      t.poke = resolve;
      t.timer = setTimeout(resolve, t.wait);
    });
    t.poke = null;
  }
}

/// What a command run in a terminal made or changed, read into Code once
/// the terminal has gone quiet: a second after the last it printed.
let termSettling = 0;
function termSettle() {
  clearTimeout(termSettling);
  termSettling = setTimeout(() => {
    if (state.view !== 'chat' || shownPane() !== 'code') return;
    loadTree();
    refreshTabs();
  }, 1000);
}

/// Read again now: something was typed, and its echo is on its way.
function termPoke(t) {
  t.wait = 16;
  if (t.poke) {
    clearTimeout(t.timer);
    t.poke();
  }
}

/// Type `data` into it. One write at a time and in order, whatever was
/// typed meanwhile sent together with the next.
function termSend(t, data) {
  const info = termInfo(t.id);
  if (!info || !info.running) return;
  t.out += data;
  if (!t.writing) termFlush(t);
}

async function termFlush(t) {
  t.writing = true;
  while (t.out) {
    const data = t.out;
    t.out = '';
    try {
      await call('term.write', { id: t.id, data });
    } catch (error) {
      t.out = '';
      break;
    }
    termPoke(t);
  }
  t.writing = false;
}

/// A paste: its line breaks as Enter presses, and fenced when the program
/// asked to be told a paste from typing.
function termPaste(t, text) {
  const typed = text.replace(/\r\n?/g, '\n').replace(/\n/g, '\r');
  termSend(t, t.screen.modes.paste ? `\x1b[200~${typed}\x1b[201~` : typed);
}

function termEnded(t, status) {
  const info = termInfo(t.id);
  if (info) {
    info.running = false;
    info.status = status;
  }
  render();
}

/// Close it, and end what runs in it.
async function termClose(id) {
  const t = terms.get(id);
  terms.delete(id);
  if (t && t.view) t.view.remove();
  const list = state.term.list.filter((one) => one.id !== id);
  state.term.list = list;
  if (state.term.active === id) state.term.active = list.length ? list[list.length - 1].id : 0;
  render();
  try { await call('term.close', { id }); } catch (error) { /* gone already */ }
}

/// What a page loaded again finds: the terminals the core still has, read
/// again from the oldest of what each printed.
async function termSync() {
  if (state.term.synced) return;
  state.term.synced = true;
  try {
    const got = await call('term.list');
    for (const one of got.terminals || []) {
      if (!terms.has(one.id)) termAdopt(one, termSize || { cols: 100, rows: 30 }, 0);
    }
    if (!state.term.active && state.term.list.length) state.term.active = state.term.list[0].id;
  } catch (error) { /* none to find */ }
}

/// The terminal pane as it opens: the ones there are, or a new one.
async function enterTerminal() {
  await termSync();
  if (!state.term.list.length) await guard(termOpen);
  else render();
}

// --- drawing one ----------------------------------------------------------------------

/// The size the last terminal shown fits, for the next one opened.
let termSize = null;

/// Draw `t` again soon, on the screen's next frame.
function termDraw(t) {
  const info = termInfo(t.id);
  if (info && info.title !== t.screen.title) {
    info.title = t.screen.title;
    render();
  }
  if (t.view) t.view.draw();
}

/// The element a terminal is shown in, and everything it listens to. Made
/// the first time it is shown, and kept until it is closed.
function termElement(t) {
  const root = document.createElement('div');
  root.className = 'term';
  root.innerHTML = `<div class="term-scroll"><div class="term-back"></div><div class="term-rows"></div></div>
    <textarea class="term-input" aria-label="Terminal" autocomplete="off" autocapitalize="off" autocorrect="off"
              spellcheck="false" wrap="off"></textarea>
    <div class="term-measure term-row" aria-hidden="true"><span>${'W'.repeat(40)}</span></div>`;
  const scroll = root.querySelector('.term-scroll');
  const back = root.querySelector('.term-back');
  const rowsEl = root.querySelector('.term-rows');
  const input = root.querySelector('.term-input');
  const measure = root.querySelector('.term-measure');
  const screen = t.screen;
  const mac = (state.snapshot.platform || '') === 'macos';
  let rowEls = [];
  let backCount = 0;
  let cursorRow = -1;
  let queued = false;
  let cell = { w: 8, h: 17 };

  const atBottom = () => scroll.scrollTop + scroll.clientHeight >= scroll.scrollHeight - 4;
  const toBottom = () => { scroll.scrollTop = scroll.scrollHeight; };

  function paint(all) {
    const wasAtBottom = atBottom();
    const gone = screen.gone();
    if (gone.cleared) {
      back.textContent = '';
      backCount = 0;
    }
    if (gone.lines.length) {
      back.insertAdjacentHTML('beforeend', gone.lines.map((line) => `<div class="term-row">${termLineHtml(line, -1)}</div>`).join(''));
      backCount += gone.lines.length;
      while (backCount > TERM_BACK && back.firstChild) {
        back.firstChild.remove();
        backCount--;
      }
    }
    back.hidden = screen.modes.alt;
    const c = screen.cursor();
    const nowRow = c.shown ? c.y : -1;
    const { whole, rows } = screen.changes();
    if (all || whole || rowEls.length !== screen.rows) {
      let html = '';
      for (let r = 0; r < screen.rows; r++) html += `<div class="term-row">${termLineHtml(screen.line(r), r === nowRow ? c.x : -1)}</div>`;
      rowsEl.innerHTML = html;
      rowEls = Array.from(rowsEl.children);
    } else {
      const redo = new Set(rows);
      if (cursorRow >= 0) redo.add(cursorRow);
      if (nowRow >= 0) redo.add(nowRow);
      for (const r of redo) {
        if (r < rowEls.length) rowEls[r].innerHTML = termLineHtml(screen.line(r), r === nowRow ? c.x : -1);
      }
    }
    cursorRow = nowRow;
    if (wasAtBottom) toBottom();
  }

  function draw() {
    if (queued) return;
    queued = true;
    requestAnimationFrame(() => {
      queued = false;
      paint(false);
    });
  }

  /// As many columns and rows as fit, told to the shell when they change.
  function fit() {
    const box = scroll.getBoundingClientRect();
    const span = measure.firstChild.getBoundingClientRect();
    const high = measure.getBoundingClientRect().height;
    if (!span.width || !high || !box.width || !box.height) return;   // not on screen
    cell = { w: span.width / 40, h: high };
    const style = getComputedStyle(scroll);
    const across = box.width - parseFloat(style.paddingLeft) - parseFloat(style.paddingRight) - (scroll.offsetWidth - scroll.clientWidth);
    const down = box.height - parseFloat(style.paddingTop) - parseFloat(style.paddingBottom);
    const cols = Math.max(20, Math.floor(across / cell.w));
    const rows = Math.max(4, Math.floor(down / cell.h));
    termSize = { cols, rows };
    if (cols === screen.cols && rows === screen.rows) return;
    screen.resize(cols, rows);
    paint(true);
    call('term.resize', { id: t.id, cols, rows }).catch(() => {});
  }

  // --- what is typed --------------------------------------------------------------
  const send = (data) => {
    termSend(t, data);
    toBottom();
  };
  const selected = () => {
    const chosen = window.getSelection ? window.getSelection() : null;
    return chosen && !chosen.isCollapsed && root.contains(chosen.anchorNode) ? chosen.toString() : '';
  };
  const copy = (text) => {
    if (navigator.clipboard && navigator.clipboard.writeText) navigator.clipboard.writeText(text).catch(() => {});
  };
  input.addEventListener('keydown', (event) => {
    if (event.isComposing || event.keyCode === 229) return;
    const key = event.key.length === 1 ? event.key.toLowerCase() : event.key;
    const command = mac ? event.metaKey : event.ctrlKey;
    // Copying: Ctrl+Shift+C, or Ctrl+C with something chosen (Cmd+C on a
    // Mac). Ctrl+C with nothing chosen is the interrupt, as ever.
    if (command && key === 'c' && (mac || event.shiftKey || selected())) {
      event.preventDefault();
      event.stopPropagation();
      const text = selected();
      if (text) copy(text);
      if (!mac && !event.shiftKey && window.getSelection) window.getSelection().removeAllRanges();
      return;
    }
    // Pasting: Ctrl+V (Cmd+V on a Mac) is the webview's own paste, which
    // arrives as a paste event. Ctrl+Shift+V is asked of the clipboard.
    if (command && key === 'v') {
      event.stopPropagation();
      if (!mac && event.shiftKey && navigator.clipboard && navigator.clipboard.readText) {
        event.preventDefault();
        navigator.clipboard.readText().then((text) => { if (text) termPaste(t, text); }, () => {});
      }
      return;
    }
    if (mac && event.metaKey) return;   // the window's own keys
    if (event.shiftKey && (event.key === 'PageUp' || event.key === 'PageDown')) {
      event.preventDefault();
      scroll.scrollTop += (event.key === 'PageUp' ? -1 : 1) * (scroll.clientHeight - cell.h);
      return;
    }
    const data = termKey(event, screen.modes, mac);
    if (data === null) return;
    // The shell's, not the window's: Ctrl+U is a line wiped here, not a
    // file to attach.
    event.preventDefault();
    event.stopPropagation();
    send(data);
  });
  input.addEventListener('input', (event) => {
    if (event.isComposing) return;
    const text = input.value;
    input.value = '';
    if (text) send(text.replace(/\r?\n/g, '\r'));
  });
  input.addEventListener('compositionend', (event) => {
    input.value = '';
    if (event.data) send(event.data);
  });
  input.addEventListener('paste', (event) => {
    event.preventDefault();
    event.stopPropagation();
    const text = event.clipboardData ? event.clipboardData.getData('text/plain') : '';
    if (text) {
      termPaste(t, text);
      toBottom();
    }
  });
  input.addEventListener('focus', () => {
    root.classList.add('focused');
    if (screen.modes.focus) termSend(t, '\x1b[I');
    draw();
  });
  input.addEventListener('blur', () => {
    root.classList.remove('focused');
    if (screen.modes.focus) termSend(t, '\x1b[O');
  });

  // --- the mouse ------------------------------------------------------------------
  //
  // A click puts the keyboard here unless it chose some text. A program that
  // asked for the mouse is told what it does, unless Shift is held, which
  // chooses text as ever.
  const cellAt = (event) => {
    const box = rowsEl.getBoundingClientRect();
    return {
      cx: Math.max(0, Math.min(screen.cols - 1, Math.floor((event.clientX - box.left) / cell.w))),
      cy: Math.max(0, Math.min(screen.rows - 1, Math.floor((event.clientY - box.top) / cell.h))),
    };
  };
  const report = (button, event, release) => {
    const { cx, cy } = cellAt(event);
    const mods = (event.shiftKey ? 4 : 0) + (event.altKey ? 8 : 0) + (event.ctrlKey ? 16 : 0);
    if (screen.modes.sgrMouse) return `\x1b[<${button + mods};${cx + 1};${cy + 1}${release ? 'm' : 'M'}`;
    if (cx > 222 || cy > 222) return '';
    return `\x1b[M${String.fromCharCode(32 + (release ? 3 : button) + mods, 33 + cx, 33 + cy)}`;
  };
  let held = -1;
  rowsEl.addEventListener('mousedown', (event) => {
    if (!screen.modes.mouse || event.shiftKey) return;
    event.preventDefault();
    input.focus({ preventScroll: true });
    held = Math.min(event.button, 2);
    termSend(t, report(held, event, false));
  });
  rowsEl.addEventListener('mouseup', (event) => {
    if (!screen.modes.mouse || event.shiftKey || held < 0) return;
    termSend(t, report(held, event, true));
    held = -1;
  });
  rowsEl.addEventListener('mousemove', (event) => {
    const m = screen.modes.mouse;
    if ((m === 1002 && held >= 0) || m === 1003) termSend(t, report((held < 0 ? 3 : held) + 32, event, false));
  });
  scroll.addEventListener('wheel', (event) => {
    if (screen.modes.mouse && !event.shiftKey) {
      event.preventDefault();
      termSend(t, report(event.deltaY < 0 ? 64 : 65, event, false));
    } else if (screen.modes.alt) {
      // A program on the other screen has nothing above it to scroll to:
      // the wheel moves through what it shows, as arrows.
      event.preventDefault();
      termSend(t, (event.deltaY < 0 ? '\x1b[A' : '\x1b[B').repeat(3));
    }
  }, { passive: false });
  root.addEventListener('mouseup', () => {
    if (!selected()) input.focus({ preventScroll: true });
  });

  const watcher = typeof ResizeObserver === 'function' ? new ResizeObserver(() => fit()) : null;
  if (watcher) watcher.observe(scroll);

  return {
    root,
    draw,
    /// It is on screen: the right size, drawn whole, and taking the keys.
    shown() {
      fit();
      paint(true);
      toBottom();
      input.focus({ preventScroll: true });
    },
    remove() {
      if (watcher) watcher.disconnect();
      root.remove();
    },
  };
}

/// After each draw, the terminal on screen is put in the place drawn for it.
afterDraw.push(() => {
  if (typeof document.querySelector !== 'function') return;
  const host = document.querySelector('.term-host');
  if (!host) return;
  const t = terms.get(Number(host.getAttribute('data-term')));
  if (!t) return;
  if (!t.view) t.view = termElement(t);
  if (host.firstChild !== t.view.root) {
    host.textContent = '';
    host.appendChild(t.view.root);
    t.view.shown();
  }
});

// --- the pane -------------------------------------------------------------------------

/// The terminals' tabs, and the one chosen. In the Terminal pane, and under
/// the editor in Code.
function termPane() {
  const s = state.term;
  const tabs = s.list.map((one) => `<div class="term-tab${one.id === s.active ? ' here' : ''}" data-key="term-${one.id}">
      <button class="term-tab-name" data-act="term-pick" data-id="${one.id}" title="${escape(one.cwd || '')}">${
        escape(termName(one))}${one.running ? '' : ' (ended)'}</button>
      <button class="term-tab-close" data-act="term-close" data-id="${one.id}" title="Close"
              aria-label="Close ${escape(termName(one))}">${CODE_ICONS.close}</button></div>`).join('');
  const active = s.list.find((one) => one.id === s.active);
  const ended = active && !active.running ? `<div class="term-ended">${apart(
      typeof active.status === 'number' ? `Ended with status ${active.status}` : 'Ended',
      '<button class="link" data-act="term-new">Open another</button>')}</div>` : '';
  const body = active
    ? `<div class="term-host" data-term="${active.id}" data-sig="term-${active.id}"></div>${ended}`
    : `<div class="term-empty">${s.error ? `<div class="term-error">${escape(s.error)}</div>` : ''}
        <button class="action" data-act="term-new">Open a terminal</button></div>`;
  return `<div class="term-pane">
      <div class="term-bar">${tabs}<button class="link term-new" data-act="term-new" title="Open another terminal">New</button></div>
      ${body}</div>`;
}

actions['term-new'] = () => guard(async () => {
  try {
    await termOpen();
  } catch (error) {
    state.term.error = error.message;
    throw error;
  }
});
actions['term-pick'] = (button) => {
  state.term.active = Number(button.dataset.id);
  render();
};
actions['term-close'] = (button) => termClose(Number(button.dataset.id));
