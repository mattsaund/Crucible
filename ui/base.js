// SPDX-License-Identifier: MIT
//
// What every view stands on: the one door to the engine, the state, and the
// loop that turns the state into the screen.
//
// The shape of the page is the simplest one that works. A view is a function
// from the state to a string of markup, and nothing else: it attaches no
// listeners and touches no element. Drawing is taking that string and making
// the document match it. Everything a person can do is an attribute on the
// thing they do it to -- data-act="send" -- handled by one listener at the
// top of the document, which looks the name up in a table.
//
// That is three ideas and they buy a lot. A view can be tested without a
// browser, because it is a pure function. Nothing is ever wired to an element
// that has since been replaced, because nothing is wired to elements. And a
// redraw does not cost what is on screen: the document is patched toward the
// new markup rather than rebuilt, so the box being typed in keeps its caret,
// the selection survives, and a list that did not change is not touched.

// --- the one door ---------------------------------------------------------
//
// window.rpc is bound from C++ and takes a JSON request. Everything below
// only knows about this, which is what keeps the interface from growing a
// second way to reach the engine.
async function call(method, params) {
  const reply = JSON.parse(await window.rpc(JSON.stringify({ method, params: params || {} })));
  if (!reply.ok) throw new Error(reply.error);
  return reply.result;
}

// --- the state ------------------------------------------------------------
//
// Everything the page knows. The snapshot is pushed by the engine; the rest
// is fetched when a view that needs it opens, because none of it changes
// without this page having asked for the change.
const state = {
  view: 'chat', lastView: 'chat', settingsPage: 'general',
  snapshot: { experts: [], turns: [], project: { open: false } },
  error: '',

  // Fetched, and null until it has been.
  config: null, models: null, providers: null, runtimes: null, devices: null,
  trainer: null, flavors: null, about: null, history: null, recipes: null,

  // Long jobs, as last reported.
  build: null, install: null, run: null,

  // What is open over the view, or null. One at a time: a modal that opens
  // another is replaced by it and comes back when that one closes.
  modal: null, modalStack: [],

  // Per-view scratch: which card is expanded, which recipe is open.
  open: {},

  // How wide the side menu is, in rem. Per viewer and per machine, so it
  // belongs in browser storage rather than in the config file -- it is a
  // window shape, not a setting about how Crucible works.
  sidebar: 15,
  composerRows: 1,
};

const SIDEBAR_MIN = 13;     // rem; the narrowest it will rest at
const SIDEBAR_SHUT = 8;     // rem; drag below this and it closes

/// Browser storage, for the things that are about this window rather than
/// about Crucible. It can be missing or refuse -- a private window, blocked
/// site data -- and the page has to draw correctly without it.
const remember = {
  get(key, fallback) {
    try {
      const raw = localStorage.getItem('crucible.' + key);
      return raw === null ? fallback : JSON.parse(raw);
    } catch (e) { return fallback; }
  },
  set(key, value) {
    try { localStorage.setItem('crucible.' + key, JSON.stringify(value)); } catch (e) { /* fine */ }
  },
};

// --- small things said once ------------------------------------------------

/// "1 turn", "2 turns". Said once because it is the same decision every
/// time, and the places that inlined it did not all get it right.
const count = (n, one, many) => `${n} ${n === 1 ? one : (many || one + 's')}`;

/// A byte count in the largest unit that keeps it readable.
function bytes(n) {
  if (!n || n < 0) return '0 B';
  const units = ['B', 'KB', 'MB', 'GB', 'TB'];
  let at = 0;
  let value = n;
  while (value >= 1024 && at < units.length - 1) { value /= 1024; at += 1; }
  return `${value >= 100 || at === 0 ? Math.round(value) : value.toFixed(1)} ${units[at]}`;
}

/// A token count that fits in a status line: 950, 12.4k, 1.2M.
function compact(n) {
  if (!n) return '0';
  if (n < 1000) return String(n);
  if (n < 1000000) return `${(n / 1000).toFixed(n < 10000 ? 1 : 0)}k`;
  return `${(n / 1000000).toFixed(1)}M`;
}

/// Seconds as a length of time somebody reads: "45s", "12 min", "2h 05m".
function span(seconds) {
  const s = Math.max(0, Math.round(seconds || 0));
  if (s < 60) return `${s}s`;
  if (s < 3600) return `${Math.round(s / 60)} min`;
  return `${Math.floor(s / 3600)}h ${String(Math.round((s % 3600) / 60)).padStart(2, '0')}m`;
}

/// A unix time as how long ago it was.
function ago(unix) {
  if (!unix) return '';
  const s = Math.max(0, Date.now() / 1000 - unix);
  if (s < 90) return 'just now';
  if (s < 3600) return `${Math.round(s / 60)} minutes ago`;
  if (s < 86400 * 2) return `${Math.round(s / 3600)} hours ago`;
  return `${Math.round(s / 86400)} days ago`;
}

/// Read `path` ("gpu.mode") out of an object, or undefined.
function at(root, path) {
  return path.split('.').reduce((node, key) => (node == null ? undefined : node[key]), root);
}

/// The object that sets `path` to `value` and nothing else: the patch
/// config.set merges.
function patchOf(path, value) {
  return path.split('.').reduceRight((inner, key) => ({ [key]: inner }), value);
}

/// An attribute list from an object, escaped. False and null are left out,
/// true is written bare -- which is what HTML means by a boolean attribute.
function attrs(map) {
  return Object.entries(map || {}).map(([name, value]) => {
    if (value === false || value == null) return '';
    return value === true ? ` ${name}` : ` ${name}="${escape(value)}"`;
  }).join('');
}

// --- making the document match -----------------------------------------------

/// Whether two nodes are the same thing in two versions of the page, and so
/// worth patching rather than replacing.
function sameNode(a, b) {
  if (a.nodeType !== b.nodeType) return false;
  if (a.nodeType !== 1) return true;
  return a.tagName === b.tagName && a.id === b.id
      && a.getAttribute('data-key') === b.getAttribute('data-key');
}

/// Make `from` look like `to`, changing as little as possible.
///
/// Children are matched in order. That is all the cleverness there is, and
/// it is enough: the lists here grow at the end and are otherwise replaced,
/// and a mismatch costs one replaced node rather than a wrong screen.
///
/// Two things are left alone on purpose. A subtree whose data-sig matches is
/// not descended into -- a finished turn of the transcript is the same
/// markup forever, and comparing it sixty times a second is the whole cost
/// of a long conversation. And the element being typed in keeps its value:
/// the state caught up with the keystroke, not the other way about.
function morph(from, to) {
  if (from.nodeType !== 1) {
    if (from.nodeValue !== to.nodeValue) from.nodeValue = to.nodeValue;
    return;
  }
  const sig = to.getAttribute('data-sig');
  if (sig !== null && sig === from.getAttribute('data-sig')) return;

  for (const { name, value } of Array.from(to.attributes)) {
    if (from.getAttribute(name) !== value) from.setAttribute(name, value);
  }
  for (const { name } of Array.from(from.attributes)) {
    if (!to.hasAttribute(name)) from.removeAttribute(name);
  }

  const tag = from.tagName;
  if (tag === 'INPUT' || tag === 'TEXTAREA' || tag === 'SELECT') {
    // Form state lives in properties, which attributes only seed. A field
    // somebody is in the middle of is theirs; any other follows the markup.
    if (from !== document.activeElement || from.hasAttribute('data-follow')) {
      if (tag === 'INPUT' && (from.type === 'checkbox' || from.type === 'radio')) {
        from.checked = to.hasAttribute('checked');
      } else if (tag === 'TEXTAREA') {
        if (from.value !== to.textContent && !from.hasAttribute('data-draft')) {
          from.value = to.textContent;
        }
      } else if (tag === 'INPUT') {
        const value = to.getAttribute('value') || '';
        if (from.value !== value && !from.hasAttribute('data-draft')) from.value = value;
      }
    }
    if (tag !== 'SELECT') return;
  }

  let a = from.firstChild;
  let b = to.firstChild;
  while (b) {
    const next = b.nextSibling;
    if (!a) {
      from.appendChild(b);
    } else if (sameNode(a, b)) {
      morph(a, b);
      a = a.nextSibling;
    } else {
      from.replaceChild(b, a);
      a = b.nextSibling;
    }
    b = next;
  }
  while (a) {
    const next = a.nextSibling;
    from.removeChild(a);
    a = next;
  }
  if (tag === 'SELECT' && from !== document.activeElement) {
    const chosen = Array.from(from.options).find((o) => o.hasAttribute('selected'));
    if (chosen && from.value !== chosen.value) from.value = chosen.value;
  }
}

/// Make the element `id` hold `html`.
///
/// Skipped outright when the markup is the string it was last time, which is
/// most of the time: a token arriving changes one turn and leaves the top
/// bar, the side menu and every modal exactly as they were.
const drawn = {};
function paint(id, html) {
  if (drawn[id] === html) return;
  drawn[id] = html;
  const target = document.getElementById(id);
  const wanted = target.cloneNode(false);
  wanted.innerHTML = html;
  morph(target, wanted);
}

// --- the loop ----------------------------------------------------------------

/// The views, by name. Each file that defines one adds it here.
const views = {};

/// Things to do after each draw that are about the document rather than the
/// markup: keeping the transcript at the bottom, sizing a text box to what is
/// in it. Each file adds its own.
const afterDraw = [];

let drawQueued = false;

/// Ask for a redraw. Any number of calls before the next frame make one.
function render() {
  if (drawQueued) return;
  drawQueued = true;
  requestAnimationFrame(draw);
}

function draw() {
  drawQueued = false;
  const transcript = document.getElementById('transcript');
  // Measured before the markup changes: whether the reader was at the bottom
  // is a fact about the screen they were looking at, not the one they are
  // about to be given.
  const pinned = !transcript
      || transcript.scrollHeight - transcript.scrollTop - transcript.clientHeight < 80;

  paint('top', topView());
  paint('main', (views[state.view] || views.chat)());
  paint('layer', state.modal ? modalView(state.modal) : '');

  for (const job of afterDraw) job({ pinned });
}

/// Run `work`, and say so if it fails.
///
/// The one way an action talks to the engine. A refusal becomes the line of
/// red text in the side menu rather than an exception nobody is listening
/// for -- or, when a modal is up, the error inside that modal, because that
/// is where the person is looking.
async function guard(work) {
  try {
    state.error = '';
    if (state.modal) state.modal.error = '';
    return await work();
  } catch (error) {
    if (state.modal) state.modal.error = error.message;
    else state.error = error.message;
    return undefined;
  } finally {
    render();
  }
}

// --- what a person can do ------------------------------------------------------

/// The actions, by name: what data-act, data-change, data-input, data-submit
/// and data-key refer to. Each is called with the element and the event.
const actions = {};

function dispatch(kind, event) {
  if (!event.target || !event.target.closest) return;
  const element = event.target.closest(`[data-${kind}]`);
  if (!element) return;
  const handler = actions[element.getAttribute(`data-${kind}`)];
  if (!handler) return;
  if (kind === 'submit') event.preventDefault();
  handler(element, event);
}

function listen() {
  document.addEventListener('click', (event) => dispatch('act', event));
  document.addEventListener('change', (event) => dispatch('change', event));
  document.addEventListener('input', (event) => dispatch('input', event));
  document.addEventListener('submit', (event) => dispatch('submit', event));
  document.addEventListener('keydown', (event) => {
    dispatch('key', event);
    shortcuts(event);
  });
  // `toggle` does not bubble, so it is caught on the way down instead.
  document.addEventListener('toggle', (event) => dispatch('toggle', event), true);
}

/// The value an input holds, as the type its data-kind says it is.
function valueOf(element) {
  const kind = element.getAttribute('data-kind');
  if (element.type === 'checkbox') return element.checked;
  if (kind === 'int')   return Math.round(Number(element.value) || 0);
  if (kind === 'float') return Number(element.value) || 0;
  return element.value;
}

// --- modals --------------------------------------------------------------------

/// The modal views, by kind. `modalView` looks one up; each file adds its own.
const modals = {};

function modalView(modal) {
  const view = modals[modal.kind];
  return view ? `<div class="veil" data-act="veil">${view(modal)}</div>` : '';
}

/// Open a modal over whatever is there. One that was already open waits
/// underneath and comes back when this one closes.
function openModal(modal) {
  if (state.modal) state.modalStack.push(state.modal);
  state.modal = modal;
  render();
}

function closeModal() {
  const closing = state.modal;
  state.modal = state.modalStack.pop() || null;
  if (closing && closing.onClose) closing.onClose();
  render();
}

actions.veil = (element, event) => {
  // Only a click on the veil itself: one that started on the modal and was
  // released outside it is somebody selecting text, not dismissing.
  if (event.target === element && state.modal && !state.modal.sticky) closeModal();
};
actions['modal-close'] = () => closeModal();

/// Ask a yes-or-no question. Resolves true or false.
function confirmIt({ title, body, yes, no }) {
  return new Promise((resolve) => {
    openModal({ kind: 'confirm', title, body, yes: yes || 'Yes', no: no || 'Cancel',
                answer: (value) => { closeModal(); resolve(value); },
                onClose: () => resolve(false) });
  });
}

modals.confirm = (m) => `<div class="modal small">
    <div class="head"><strong>${escape(m.title)}</strong></div>
    <div class="body-pad"><div class="lede" style="margin:0">${escape(m.body)}</div></div>
    <div class="feet">
      <button class="action" data-act="confirm-yes" data-focus>${escape(m.yes)}</button>
      <button class="action" data-act="confirm-no">${escape(m.no)}</button>
    </div></div>`;
actions['confirm-yes'] = () => { const m = state.modal; m.onClose = null; m.answer(true); };
actions['confirm-no']  = () => { const m = state.modal; m.onClose = null; m.answer(false); };

// --- choosing a file or a folder -------------------------------------------------

/// Ask for a path. Resolves to it, or to '' when nothing was chosen.
///
/// The platform's own dialog when the window has one to show, which is
/// nearly always. It is the one the person already knows, and it knows things
/// a picker drawn here never will -- the bookmarks, the mounted shares, the
/// drive plugged in a minute ago. The page's own picker is what is left for
/// a desktop with no dialog to give.
async function pickPath(wanted) {
  // Named differently from this function on purpose. A function declared at
  // the top of a script *is* a property of window, so a binding called
  // pickPath would be replaced by this one the moment the script loaded --
  // and this would then call itself until the stack ran out.
  if (window.nativeDialog && !wanted.noNative) {
    try {
      // The window answers with an object, not with text to be parsed: this
      // is its own binding and not the surface, which answers in strings so
      // that a pipe can carry them.
      const answer = await window.nativeDialog(wanted);
      if (answer && answer.supported) return answer.path || '';
    } catch (e) { /* no dialog after all: fall through to the page's own */ }
  }
  return new Promise((resolve) => {
    openModal({ kind: 'browser', wanted, listing: null, path: wanted.start || '',
                choose: (path) => { closeModal(); resolve(path); },
                onClose: () => resolve('') });
    browseTo(wanted.start || '');
  });
}

/// Ask for one path or several. Resolves to a list, empty when nothing was
/// chosen. `multiple` lets the platform's dialog take more than one; the
/// page's own picker, where there is no dialog, chooses one at a time.
async function pickPaths(wanted) {
  if (window.nativeDialog) {
    try {
      const answer = await window.nativeDialog(wanted);
      if (answer && answer.supported) {
        return (answer.paths && answer.paths.length ? answer.paths : [answer.path]).filter(Boolean);
      }
    } catch (e) { /* no dialog after all: fall through to the page's own */ }
  }
  const one = await pickPath(Object.assign({}, wanted, { noNative: true }));
  return one ? [one] : [];
}

async function browseTo(path) {
  const modal = state.modal;
  if (!modal || modal.kind !== 'browser') return;
  await guard(async () => {
    modal.listing = await call('browse', {
      path, files: !modal.wanted.folder, extensions: modal.wanted.extensions || [] });
    modal.path = modal.listing.path;
  });
}

modals.browser = (m) => {
  const l = m.listing;
  const rows = !l ? '<div class="status">Reading...</div>'
    : (l.entries.map((name) =>
        `<button data-act="browse-into" data-path="${escape(l.path + '/' + name)}">${
          escape(name)}/</button>`).join('')
      + (l.files || []).map((file) =>
        `<button data-act="browse-file" data-path="${escape(l.path + '/' + file.name)}">${
          escape(file.name)} <span class="status">${bytes(file.bytes)}</span></button>`).join(''))
      || '<div class="status">Nothing here.</div>';
  return `<div class="modal">
    <div class="head"><strong>${escape(m.wanted.title || 'Choose')}</strong></div>
    <div class="body-pad">
      <input class="crumbs" data-key="browse-typed" value="${escape(m.path)}" spellcheck="false"
             aria-label="Path. Type or paste one and press Enter.">
      <div class="listing">${rows}</div>
      ${m.error ? `<div class="bad" style="margin-top:.6rem">${escape(m.error)}</div>` : ''}
    </div>
    <div class="feet">
      ${m.wanted.folder ? '<button class="action" data-act="browse-choose">Use this folder</button>' : ''}
      <button class="action" data-act="browse-up" ${l && l.parent ? '' : 'disabled'}>Up</button>
      <button class="action" data-act="browse-home">Home</button>
      <span class="spacer"></span>
      <button class="action" data-act="modal-close">Cancel</button>
    </div></div>`;
};
actions['browse-into']   = (e) => browseTo(e.dataset.path);
actions['browse-file']   = (e) => { const m = state.modal; m.onClose = null; m.choose(e.dataset.path); };
actions['browse-up']     = () => browseTo(state.modal.listing.parent);
actions['browse-home']   = () => browseTo(state.modal.listing ? state.modal.listing.home : '');
actions['browse-choose'] = () => { const m = state.modal; m.onClose = null; m.choose(m.path); };
actions['browse-typed']  = (e, event) => { if (event.key === 'Enter') browseTo(e.value); };

// --- keeping what is fetched fresh -------------------------------------------------

/// Fetch `key` with `method` unless it is already here, and redraw when it
/// lands. `force` asks again regardless, for after something changed it.
///
/// Every one of these is a lookup the window answers on a thread of its own,
/// so a view opens at once and fills in as each arrives. A view that is drawn
/// before its data says so in words rather than waiting.
const fetching = {};
function need(key, method, force) {
  if (fetching[key] || (state[key] !== null && !force)) return;
  fetching[key] = true;
  call(method).then((result) => { state[key] = result; })
    .catch((error) => { state.error = error.message; })
    .finally(() => { fetching[key] = false; render(); });
}

/// Change the configuration and take back what it became.
///
/// Every setting on every page goes through here, the moment it is changed:
/// there are no Save buttons. A control that has been set and not yet
/// applied is a state the old window never had, and it is the one where a
/// person closes the page and loses what they thought they had done.
async function configure(patch) {
  await guard(async () => {
    const reply = await call('config.set', patch);
    state.config = await call('config');
    if (reply.warnings && reply.warnings.length) state.error = reply.warnings.join('  ');
  });
}
