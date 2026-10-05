// SPDX-License-Identifier: MIT
//
// What is on screen whichever view is showing: the bar across the top, the
// side menu, and the handful of dialogs that belong to no one view.

// --- pieces more than one view draws -------------------------------------------

const ICONS = {
  fold: `<svg viewBox="0 0 16 16" width="15" height="15" aria-hidden="true">
    <rect x="1.5" y="2.5" width="13" height="11" rx="1.5" fill="none" stroke="currentColor" stroke-width="1.2"/>
    <line x1="6" y1="2.5" x2="6" y2="13.5" stroke="currentColor" stroke-width="1.2"/></svg>`,
  gear: `<svg viewBox="0 0 24 24" width="17" height="17" aria-hidden="true" fill="none" stroke="currentColor"
    stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="3"/>
    <path d="M19.4 15a1.65 1.65 0 0 0 .33 1.82l.06.06a2 2 0 1 1-2.83 2.83l-.06-.06a1.65 1.65 0 0 0-1.82-.33 1.65 1.65 0 0 0-1 1.51V21a2 2 0 1 1-4 0v-.09A1.65 1.65 0 0 0 9 19.4a1.65 1.65 0 0 0-1.82.33l-.06.06a2 2 0 1 1-2.83-2.83l.06-.06a1.65 1.65 0 0 0 .33-1.82 1.65 1.65 0 0 0-1.51-1H3a2 2 0 1 1 0-4h.09A1.65 1.65 0 0 0 4.6 9a1.65 1.65 0 0 0-.33-1.82l-.06-.06a2 2 0 1 1 2.83-2.83l.06.06a1.65 1.65 0 0 0 1.82.33H9a1.65 1.65 0 0 0 1-1.51V3a2 2 0 1 1 4 0v.09a1.65 1.65 0 0 0 1 1.51 1.65 1.65 0 0 0 1.82-.33l.06-.06a2 2 0 1 1 2.83 2.83l-.06.06a1.65 1.65 0 0 0-.33 1.82V9a1.65 1.65 0 0 0 1.51 1H21a2 2 0 1 1 0 4h-.09a1.65 1.65 0 0 0-1.51 1z"/></svg>`,
  stop:  '<svg viewBox="0 0 16 16" width="13" height="13" aria-hidden="true"><rect x="3.5" y="3.5" width="9" height="9" rx="1" fill="currentColor"/></svg>',
  retry: '<svg viewBox="0 0 16 16" width="14" height="14" aria-hidden="true" fill="none" stroke="currentColor" stroke-width="1.4" stroke-linecap="round" stroke-linejoin="round"><path d="M13 8a5 5 0 1 1-1.6-3.7"/><path d="M13 2.5v3h-3"/></svg>',
  trash: '<svg viewBox="0 0 16 16" width="14" height="14" aria-hidden="true" fill="none" stroke="currentColor" stroke-width="1.3" stroke-linecap="round" stroke-linejoin="round"><path d="M3 4.5h10M6.5 4.5V3h3v1.5M4.5 4.5l.6 8.5h5.8l.6-8.5"/></svg>',
  cloud: '<svg viewBox="0 0 16 16" width="13" height="13" aria-hidden="true" fill="none" stroke="currentColor" stroke-width="1.3" stroke-linejoin="round"><path d="M4.5 12.5a3 3 0 0 1-.4-5.97A4 4 0 0 1 11.9 7.6a2.5 2.5 0 0 1-.4 4.9z"/></svg>',
  down:  '<svg viewBox="0 0 16 16" width="13" height="13" aria-hidden="true" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"><path d="M8 3v9M4 8.5l4 4 4-4"/></svg>',
};

/// A load, as a ring that fills with the figure in the middle of it.
///
/// Beside the name of whatever is loading, on its own line: which model is
/// coming up and how far along it is are one fact, and a bar somewhere else
/// on the screen made them two.
function ring(progress) {
  const fraction = Math.max(0, Math.min(1, progress || 0));
  const radius = 12.5;
  const around = 2 * Math.PI * radius;
  return `<svg class="ring" viewBox="0 0 30 30" width="34" height="34" role="img"
      aria-label="${Math.round(fraction * 100)} percent loaded">
    <circle class="ring-track" cx="15" cy="15" r="${radius}"/>
    <circle class="ring-fill" cx="15" cy="15" r="${radius}"
            stroke-dasharray="${around.toFixed(2)}"
            stroke-dashoffset="${(around * (1 - fraction)).toFixed(2)}"/>
    <text x="15" y="15.5">${Math.round(fraction * 100)}%</text></svg>`;
}

/// A file name out of whatever a seat's model was written as.
const fileName = (reference) => (reference ? String(reference).split(/[\\/]/).pop() : '');

/// The name on the roster, not the id the router returns.
///
/// They differ by capitalization today and could differ by more tomorrow;
/// the id is a key and the name is what somebody chose to call the seat.
function expertName(id) {
  const seat = (state.snapshot.experts || []).find((e) => e.id === id);
  return seat ? seat.name : id;
}

/// A dropdown of everything a seat can be pointed at.
///
/// Four groups, in the order of how much of it is yours: nothing, what was
/// made here, what is on this machine, and what is answered somewhere else.
/// The value is "provider|model" for the last group and the file otherwise,
/// which `splitModel` takes apart again.
function modelSelect(selected, provider, extra, localOnly) {
  const chosen = provider ? `${provider}|${selected}` : (selected || '');
  const option = (value, label, title) =>
    `<option value="${escape(value)}"${value === chosen ? ' selected' : ''}${
      title ? ` title="${escape(title)}"` : ''}>${escape(label)}</option>`;

  const files = state.models ? state.models.models : [];
  const made  = files.filter((m) => m.made_here);
  const local = files.filter((m) => !m.made_here);
  const known = new Set(files.flatMap((m) => [m.name, m.path]));

  let html = option('', '(none)');
  if (made.length) {
    html += `<optgroup label="Made in Crucible">${made.map((m) =>
      option(m.path, `${m.name}  ·  ${bytes(m.bytes)}`, m.purpose)).join('')}</optgroup>`;
  }
  if (local.length) {
    html += `<optgroup label="On this machine">${local.map((m) =>
      option(m.name, `${m.name}  ·  ${bytes(m.bytes)}`)).join('')}</optgroup>`;
  }
  // `localOnly` is for the delegator, which has to be a file here.
  for (const p of (state.providers && !localOnly ? state.providers.providers : [])) {
    const offered = p.models.slice();
    if (provider === p.id && selected && !offered.includes(selected)) offered.unshift(selected);
    if (offered.length) {
      html += `<optgroup label="${escape(p.name)}${p.on_your_network ? '' : '  (leaves this machine)'}">${
        offered.map((m) => option(`${p.id}|${m}`, m)).join('')}</optgroup>`;
    }
  }
  // A file the seat names that the scan did not find: still offered, so the
  // dropdown shows what is configured rather than quietly showing "(none)".
  if (selected && !provider && !known.has(selected)) {
    html += option(selected, `${fileName(selected)}  (not found)`);
  }
  return `<select${attrs(extra)}>${html}</select>`;
}

function splitModel(value) {
  const bar = value.indexOf('|');
  return bar < 0 ? { model: value, provider: '' }
                 : { provider: value.slice(0, bar), model: value.slice(bar + 1) };
}

// --- the bar across the top ------------------------------------------------------

const TABS = [
  ['chat',    'Chat',    'Ask one question'],
  ['cook',    'Cook',    'Work one goal in passes'],
  ['create',  'Create',  'Fine-tune an expert'],
  ['history', 'History', 'Past cooks and conversations'],
];

function topView() {
  const s = state.snapshot;
  const project = s.project || { open: false };
  const tabs = TABS.map(([view, label, tip]) =>
    `<button data-act="view" data-view="${view}" title="${tip}"
             aria-current="${state.view === view}">${label}</button>`).join('');
  const update = s.update
    ? `<span class="pip" title="Crucible ${escape(s.update.latest)} is available"></span>` : '';
  // The path sits beside the button that changes it, and it is text: it can
  // be selected and copied, which a tooltip cannot.
  return `
    <button class="icon" data-act="fold" title="Show or hide the side menu"
            aria-label="Show or hide the side menu">${ICONS.fold}</button>
    <img class="flame" src="${escape(state.mark || '')}" alt="">
    <span class="mark">CRUCIBLE</span>
    <button class="project${project.open ? '' : ' none'}" data-act="open-project"
            title="${project.open ? 'Open another project' : 'Choose a folder to work in'}">${
      project.open ? escape(project.name) : 'Open Project'}</button>
    <span class="project-path" title="${escape(project.root || '')}"><bdi>${
      escape(project.display || '')}</bdi></span>
    <nav>${tabs}
      <button class="icon" data-act="gear" aria-current="${state.view === 'settings'}"
              title="${s.update ? `Settings  ·  Crucible ${escape(s.update.latest)} is available`
                                : 'Settings'}" aria-label="Settings">${ICONS.gear}${update}</button>
    </nav>`;
}

// --- the side menu ----------------------------------------------------------------

function sidebarOpen() { return state.sidebar >= SIDEBAR_SHUT; }
function sidebarWidth() { return Math.max(state.sidebar, SIDEBAR_MIN); }

function setSidebar(rem) {
  state.sidebar = Math.max(0, Math.min(rem, 34));
  remember.set('sidebar', state.sidebar);
  render();
}

/// What a seat row says when the pointer rests on it.
function seatTip(e) {
  const where = e.provider ? `${e.model}  ·  answered by ${e.provider}`
              : e.model ? e.model + (e.phase === 'missing' ? '  (missing)' : '')
              : 'no model yet';
  return `${e.blurb ? e.blurb + '\n' : ''}${where}`;
}

function sideView() {
  const s = state.snapshot;
  const experts = s.experts || [];
  const delegator = s.delegator || {};
  const loading = s.delegator_progress !== undefined;
  const routing = s.mood === 'routing' || loading;
  // Where the work is, as a line: from the delegator down to whichever seat
  // has the turn. It is the picture of what the program does, and it is the
  // one thing here that moves.
  const linked = experts.findIndex((e) => e.id === s.linked);
  // Assigned is enough to draw it as present. With the delegator set to load
  // on demand it is out of memory most of the time, and that is the design
  // working rather than a seat with nothing in it.
  const delegatorPhase = routing ? 'active' : delegator.model ? 'dormant' : 'unconfigured';

  if (!sidebarOpen()) {
    // Closed, it is a rail rather than nothing: the dots still say which seat
    // is loaded, which is the one thing worth a glance while it is folded.
    const dots = experts.map((e, i) =>
      `<button class="seat" data-act="settings-page" data-page="experts"
               data-phase="${i === linked ? 'active' : e.phase}" title="${escape(e.name)}">
         <span class="dot"></span></button>`).join('');
    return `<aside class="rail">
        <button class="seat" data-act="settings-page" data-page="general"
                data-phase="${delegatorPhase}"
                title="${escape(delegator.model || 'no delegator')}"><span class="dot"></span></button>
        <div class="rail-gap"></div>${dots}
      </aside>`;
  }

  const seats = experts.map((e, i) => {
    const classes = ['seat'];
    if (linked >= 0 && i < linked) classes.push('on-trunk');
    if (i === linked) classes.push('elbow', 'linked');
    return `<button class="${classes.join(' ')}" data-act="settings-page" data-page="experts"
        data-phase="${i === linked && e.phase !== 'loading' ? 'active' : e.phase}"
        title="${escape(seatTip(e))}">
      <span class="dot"></span><span class="name">${escape(e.name)}</span>
      ${e.provider ? `<span class="cloud" title="Answered by ${escape(e.provider)}">${ICONS.cloud}</span>` : ''}
      ${e.phase === 'loading' ? ring(e.progress) : ''}
    </button>`;
  }).join('') || '<div class="status">None yet.</div>';

  const status = state.error || s.status || '';
  const loaded = !!s.resident || (s.delegator_ready && !!delegator.model);
  // Eject is sized to its own word rather than to the panel: it is an
  // occasional action, and a button stretched across a side menu that can be
  // dragged to four hundred pixels reads as the most important thing here.
  return `<aside style="width:${sidebarWidth()}rem">
      <div class="side-status${state.error ? ' bad' : s.busy ? ' busy' : ''}" title="${escape(status)}">${
        escape(status) || '&nbsp;'}</div>
      <div class="roster-scroll">
        <h2${linked >= 0 ? ' class="on-trunk-gap"' : ''}>DELEGATOR</h2>
        <button class="seat${routing ? ' linked' : ''}${linked >= 0 ? ' trunk-start' : ''}"
                data-act="settings-page" data-page="general" data-phase="${delegatorPhase}"
                title="${escape(delegator.model ? 'Reads the prompt, names the expert.\n' + delegator.model
                                                : 'No delegator: prompts are routed on keywords.')}">
          <span class="dot"></span><span class="name">${escape(delegator.model || '(none)')}</span>
          ${loading ? ring(s.delegator_progress) : ''}
        </button>
        <h2 class="experts-head${linked >= 0 ? ' on-trunk' : ''}">EXPERTS</h2>${seats}
        <button class="action wide" data-act="settings-page" data-page="experts">Manage experts</button>
      </div>
      <div class="roster-foot">
        <button class="action" data-act="eject" ${loaded ? '' : 'disabled'}
                title="${loaded ? 'Unload every model' : 'Nothing is loaded'}">Eject</button>
      </div>
    </aside>
    <div class="splitter" id="splitter" title="Drag to resize. Drag to the edge to close."></div>`;
}

actions.fold = () => setSidebar(sidebarOpen() ? 0 : Math.max(remember.get('sidebar-open', 15), SIDEBAR_MIN));
actions.eject = () => guard(() => call('release', { all: true }));

/// Dragging the edge of the side menu. Bound once to the document rather
/// than to the splitter, which is redrawn while it is being dragged.
function listenForSplitters() {
  document.addEventListener('pointerdown', (down) => {
    const handle = down.target.closest && down.target.closest('#splitter, #composer-splitter');
    if (!handle) return;
    down.preventDefault();
    const rem = parseFloat(getComputedStyle(document.documentElement).fontSize) || 16;
    const sideways = handle.id === 'splitter';
    const from = sideways ? down.clientX : down.clientY;
    const start = sideways ? (sidebarOpen() ? sidebarWidth() : 0) : state.composerRows;
    document.body.classList.add(sideways ? 'dragging-x' : 'dragging-y');
    const move = (event) => {
      if (sideways) {
        setSidebar(start + (event.clientX - from) / rem);
      } else {
        // Up makes it taller. A line of this text is 1.6rem.
        state.composerRows = Math.max(1, Math.min(18, Math.round(start + (from - event.clientY) / (rem * 1.6))));
        remember.set('composer-rows', state.composerRows);
        render();
      }
    };
    const up = () => {
      document.removeEventListener('pointermove', move);
      document.removeEventListener('pointerup', up);
      document.body.classList.remove('dragging-x', 'dragging-y');
      if (!sideways) return;
      // A deliberate overshoot closes it, so trimming the width cannot slam
      // it shut on one pixel of movement.
      if (state.sidebar < SIDEBAR_SHUT) setSidebar(0);
      else {
        if (state.sidebar < SIDEBAR_MIN) setSidebar(SIDEBAR_MIN);
        remember.set('sidebar-open', state.sidebar);
      }
    };
    document.addEventListener('pointermove', move);
    document.addEventListener('pointerup', up);
  });
}

// --- moving between views ------------------------------------------------------------

/// Switch to a view, then fill it in.
///
/// The draw comes first and the fetches follow, each redrawing as it lands.
/// A view that is drawn before its data says "Reading..." rather than making
/// the window wait for a disk or a network that is none of its business.
function enter(view, page) {
  if (view !== 'settings' && state.view !== 'settings') state.lastView = state.view;
  if (view === 'settings' && state.view !== 'settings') state.lastView = state.view;
  state.view = view;
  if (page) state.settingsPage = page;
  state.error = '';
  render();
  if (entering[view]) entering[view]();
}

/// What each view fetches when it opens. Each file adds its own.
const entering = {};

actions.view = (element) => enter(element.dataset.view);
actions['settings-page'] = (element) => enter('settings', element.dataset.page);
// The gear is a toggle: pressing it while in Settings goes back to wherever
// you were, which is what makes Settings a place you visit rather than a
// place you have to find your way out of.
actions.gear = () => enter(state.view === 'settings' ? (state.lastView || 'chat') : 'settings');

function shortcuts(event) {
  if (!(event.ctrlKey || event.metaKey) || event.altKey) return;
  const tab = { 1: 'chat', 2: 'cook', 3: 'create', 4: 'history' }[event.key];
  if (tab) { event.preventDefault(); enter(tab); }
  if (event.key === ',') { event.preventDefault(); actions.gear(); }
}

// --- which folder ----------------------------------------------------------------------

actions['open-project'] = async () => {
  const project = state.snapshot.project || {};
  const path = await pickPath({ folder: true, title: 'Choose a folder to work in',
                                start: project.root || '' });
  if (path) await guard(() => call('project.open', { path }));
};

/// The folder question, when one is waiting. Asked by the session and shown
/// here: the page learns of it from the snapshot, and so does not have to
/// remember that it asked.
function trustWatch() {
  const asking = (state.snapshot.project || {}).pending_trust;
  const showing = state.modal && state.modal.kind === 'trust';
  if (asking && !showing) openModal({ kind: 'trust', path: asking, sticky: true });
  else if (!asking && showing) closeModal();
}

modals.trust = (m) => `<div class="modal">
    <div class="head"><strong>Trust this folder?</strong></div>
    <div class="body-pad">
      <div class="crumbs">${escape(m.path)}</div>
      <div class="status">Experts may read, write and run commands here. Paths
        outside it are refused &mdash; a command they run is not.</div>
    </div>
    <div class="feet">
      <button class="action" data-act="trust-yes" data-focus>Trust and open</button>
      <button class="action" data-act="trust-no">Cancel</button>
    </div></div>`;
actions['trust-yes'] = () => guard(() => call('trust.answer', { trusted: true }));
actions['trust-no']  = () => guard(() => call('trust.answer', { trusted: false }));

// --- a new expert -----------------------------------------------------------------------

/// The quick way to an expert: a name, what it is for, and a model that
/// already exists -- a file here, or one at a provider. Fine-tuning one from
/// scratch is the Create tab, and this dialog says so.
modals['new-expert'] = (m) => `<div class="modal">
    <div class="head"><strong>New expert</strong>
      <div class="status">A seat the delegator can route to, answered by a model you already have.</div></div>
    <div class="body-pad">
      <div class="field"><label for="ne-name">Expert name</label>
        <input id="ne-name" data-input="ne-field" data-field="name" data-draft data-focus
               placeholder="Rust Async, Tax Law, Kubernetes" value="${escape(m.name)}"></div>
      <div class="field"><label for="ne-what">Describe what the expert is trained in</label>
        <textarea id="ne-what" rows="4" data-input="ne-field" data-field="description" data-draft>${
          escape(m.description)}</textarea>
        <div class="hint">The delegator routes on this, so name the things it should take.</div></div>
      <div class="field"><label for="ne-model">Model</label>
        <div class="row">${modelSelect(m.model, m.provider,
            { id: 'ne-model', 'data-change': 'ne-model' })}
          <button class="action" type="button" data-act="ne-browse">Browse</button></div>
        <div class="hint">One you fine-tuned in Create, a file on this machine, or a model at a
          provider you have added. It can be left empty and chosen later.</div></div>
      ${m.error ? `<div class="bad">${escape(m.error)}</div>` : ''}
    </div>
    <div class="feet">
      <button class="action" data-act="ne-add">Add expert</button>
      <button class="action" data-act="modal-close">Cancel</button>
      <span class="spacer"></span>
      <button class="link" data-act="ne-train">Fine-tune one instead</button>
    </div></div>`;

actions['new-expert'] = () => {
  need('models', 'models');
  need('providers', 'providers');
  openModal({ kind: 'new-expert', name: '', description: '', model: '', provider: '' });
};
actions['ne-field'] = (e) => { state.modal[e.dataset.field] = e.value; };
actions['ne-model'] = (e) => { Object.assign(state.modal, splitModel(e.value)); render(); };
actions['ne-browse'] = async () => {
  const modal = state.modal;
  const path = await pickPath({ title: 'Choose a model file', filter: 'GGUF models',
                                extensions: ['.gguf'],
                                start: state.models ? state.models.directory : '' });
  if (path) { modal.model = path; modal.provider = ''; render(); }
};
actions['ne-add'] = () => guard(async () => {
  const m = state.modal;
  await call('expert.add', { name: m.name, description: m.description,
                             model: m.model, provider: m.provider });
  state.config = await call('config');
  closeModal();
});
actions['ne-train'] = () => { closeModal(); enter('create'); actions['recipe-new'](); };

// --- start ----------------------------------------------------------------------------------

/// The engine pushes rather than the page polling: it already knows when
/// something changed, and a timer would either lag behind that or burn a
/// redraw a second doing nothing.
window.crucibleSnapshot = (snapshot) => {
  state.snapshot = snapshot;
  trustWatch();
  for (const job of onSnapshot) job();
  render();
};

/// Things to do when the engine says something changed, beyond redrawing.
/// A long job that pokes the window as it goes is followed from here.
const onSnapshot = [];

function start() {
  state.sidebar = remember.get('sidebar', 15);
  state.composerRows = remember.get('composer-rows', 1);
  state.mark = document.body.getAttribute('data-mark') || '';
  listen();
  listenForSplitters();
  guard(async () => {
    // The config comes down with the first snapshot rather than when the
    // settings screen is first opened: the chat view reads it to decide what
    // to say when there is nothing to chat with.
    const [snapshot, config] = await Promise.all([call('snapshot'), call('config')]);
    state.snapshot = snapshot;
    state.config = config;
    trustWatch();
  });
  // What the empty chat needs to know before it can say what is missing.
  need('runtimes', 'runtimes');
}
