// SPDX-License-Identifier: MIT
//
// What is on screen whichever view is showing: the bar across the top, the
// side menu, and the handful of dialogs that belong to no one view.

// --- pieces more than one view draws -------------------------------------------

const ICONS = {
  fold: `<svg viewBox="0 0 16 16" width="15" height="15" aria-hidden="true">
    <rect x="1.5" y="2.5" width="13" height="11" rx="1.5" fill="none" stroke="currentColor" stroke-width="1.2"/>
    <line x1="6" y1="2.5" x2="6" y2="13.5" stroke="currentColor" stroke-width="1.2"/></svg>`,
  foldRight: `<svg viewBox="0 0 16 16" width="15" height="15" aria-hidden="true">
    <rect x="1.5" y="2.5" width="13" height="11" rx="1.5" fill="none" stroke="currentColor" stroke-width="1.2"/>
    <line x1="10" y1="2.5" x2="10" y2="13.5" stroke="currentColor" stroke-width="1.2"/></svg>`,
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
///
/// The arc is turned to start at twelve o'clock with the SVG's own transform
/// attribute, about the circle's own center. It used to be a CSS rotation, and
/// WebKit placed that rotation's origin somewhere else: the arc swung off the
/// circle, most of it was clipped, and what was left poked out over the name.
///
/// A load that does not say how far along it is -- MLX starting a model --
/// comes as a negative figure, and is drawn as a turning arc with no number:
/// a percentage that is not measuring anything is worse than none. Turned by
/// SVG's own animation, about the center, for the reason above.
function ring(progress) {
  const radius = 13;
  const around = 2 * Math.PI * radius;
  if (progress < 0) {
    return `<svg class="ring" viewBox="0 0 32 32" width="32" height="32" role="img" aria-label="loading">
      <circle class="ring-track" cx="16" cy="16" r="${radius}"/>
      <circle class="ring-fill" cx="16" cy="16" r="${radius}"
              stroke-dasharray="${(around / 4).toFixed(2)} ${around.toFixed(2)}">
        <animateTransform attributeName="transform" type="rotate" from="0 16 16" to="360 16 16"
                          dur="1.1s" repeatCount="indefinite"/></circle></svg>`;
  }
  const fraction = Math.max(0, Math.min(1, progress || 0));
  return `<svg class="ring" viewBox="0 0 32 32" width="32" height="32" role="img"
      aria-label="${Math.round(fraction * 100)} percent loaded">
    <circle class="ring-track" cx="16" cy="16" r="${radius}"/>
    <circle class="ring-fill" cx="16" cy="16" r="${radius}" transform="rotate(-90 16 16)"
            stroke-dasharray="${around.toFixed(2)}"
            stroke-dashoffset="${(around * (1 - fraction)).toFixed(2)}"/>
    <text x="16" y="16.5">${Math.round(fraction * 100)}%</text></svg>`;
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
/// The models folder, and then each provider's models. One folder on purpose:
/// it is where every model is chosen from and where a fine-tune made here is
/// written to, so a model put there is a model every seat can have -- the
/// delegator included -- and nothing has to be gone looking for.
///
/// The value is "provider|model" for a provider's and the file name
/// otherwise, which `splitModel` takes apart again.
function modelSelect(selected, provider, extra, localOnly) {
  const chosen = provider ? `${provider}|${selected}` : (selected || '');
  const option = (value, label, title) =>
    `<option value="${escape(value)}"${value === chosen ? ' selected' : ''}${
      title ? ` title="${escape(title)}"` : ''}>${escape(label)}</option>`;

  const files = state.models ? state.models.models : [];
  const known = new Set(files.flatMap((m) => [m.name, m.path]));

  let html = option('', '(none)');
  // The delegator scores every seat with llama.cpp, so it has to be a GGUF;
  // an MLX model can answer, not route. One this machine cannot run is
  // listed, so it is plain that it was found, and not offered.
  const offered = localOnly ? files.filter((m) => m.format !== 'mlx') : files;
  const mlxWhy = state.models ? state.models.mlx_unavailable || '' : '';
  if (offered.length) {
    html += `<optgroup label="Models folder">${offered.map((m) => {
      if (m.format !== 'mlx') return option(m.name, `${m.name} (${bytes(m.bytes)})`);
      const usable = !mlxWhy || m.name === chosen;
      return `<option value="${escape(m.name)}"${m.name === chosen ? ' selected' : ''}${usable ? '' : ' disabled'}
          title="${escape(mlxWhy || 'An MLX model, answered by MLX on this machine')}">${
          escape(`${m.name} (MLX, ${bytes(m.bytes)})${usable ? '' : ', cannot run here'}`)}</option>`;
    }).join('')}</optgroup>`;
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
  // A file the seat names that is not in the folder -- set before there was
  // one folder, or since moved. Still offered, so the dropdown shows what is
  // configured rather than quietly showing "(none)".
  if (selected && !provider && !known.has(selected)) {
    html += option(selected, `${fileName(selected)}  (not in the models folder)`);
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
  ['build',   'Build',   'Give it a directive: it plans, delegates, builds and ships'],
  ['create',  'Create',  'Fine-tune an expert'],
  ['history', 'History', 'Past builds and conversations'],
];

function topView() {
  const s = state.snapshot;
  const tabs = TABS.map(([view, label, tip]) =>
    `<button data-act="view" data-view="${view}" title="${tip}"
             aria-current="${state.view === view}">${label}</button>`).join('');
  const update = s.update
    ? `<span class="pip" title="Crucible ${escape(s.update.latest)} is available"></span>` : '';
  // Opening a project, or another chat, is the right-hand panel's; the bar
  // only says which folder this chat works in -- the project's, or the
  // scratch folder of a chat with none, or before its first message, where
  // that folder will go. Cut from the left when it is long, because the end
  // is the part that differs between two of them.
  const project = s.project || {};
  const folder = project.open ? project.display || project.root : project.scratch_display;
  return `
    <button class="icon" data-act="fold" title="Show or hide the side menu"
            aria-label="Show or hide the side menu">${ICONS.fold}</button>
    <img class="flame" src="${escape(state.mark || '')}" alt="">
    <span class="mark">CRUCIBLE</span>
    ${folder ? `<button type="button" class="project-path" data-act="show-folder"
        title="${escape(project.root || folder)}"><bdi>${escape(folder)}</bdi></button>` : ''}
    <nav>${tabs}
      <button class="icon" data-act="gear" aria-current="${state.view === 'settings'}"
              title="${s.update ? `Settings\nCrucible ${escape(s.update.latest)} is available`
                                : 'Settings'}" aria-label="Settings">${ICONS.gear}${update}</button>
      <button class="icon" data-act="fold-right" aria-pressed="${recentsOpen()}"
              title="Show or hide recent chats and projects"
              aria-label="Show or hide recent chats and projects">${ICONS.foldRight}</button>
    </nav>`;
}

/// The path in the top bar opens its folder in the desktop's file browser.
/// The window knows which folder that is; the page only asks.
actions['show-folder'] = () => guard(async () => {
  if (!window.showFolder) return;
  const answer = await window.showFolder();
  if (answer && answer.error) throw new Error(answer.error);
});

// --- the side menu ----------------------------------------------------------------

function sidebarOpen() { return state.sidebar >= SIDEBAR_SHUT; }
function sidebarWidth() { return Math.max(state.sidebar, SIDEBAR_MIN); }

function setSidebar(rem) {
  state.sidebar = Math.max(0, Math.min(rem, 34));
  remember.set('sidebar', state.sidebar);
  render();
}

// --- the other side: what has been done lately ----------------------------------------
//
// Conversations, newest first, across every project Crucible has been opened
// in -- and those projects. Choosing a conversation opens its project, when it
// is not the one open, and puts the conversation back with the expert's
// memory of it, so the next question carries on from where it stopped.

function recentsOpen() { return state.recentsWidth >= SIDEBAR_SHUT; }
function recentsWidth() { return Math.max(state.recentsWidth, SIDEBAR_MIN); }

function setRecents(rem) {
  state.recentsWidth = Math.max(0, Math.min(rem, 34));
  remember.set('recents-width', state.recentsWidth);
  render();
}

function recentsView() {
  if (!recentsOpen()) return '';
  const r = state.recents;
  const here = state.snapshot.session || '';
  const project = state.snapshot.project || {};
  const busy = !!state.snapshot.busy;
  // The bin shows on hover. Not while something runs: the chat being
  // deleted could be the one being written to.
  const bin = (act, data, label) => busy ? '' : `<button class="icon r-trash" data-act="${act}" ${data}
      title="${label}" aria-label="${label}">${ICONS.trash}</button>`;
  const chats = !r ? '<div class="status r-empty">Reading...</div>'
    : r.chats.length ? r.chats.map((c) => `<div class="r-row"><button class="recent${c.id === here ? ' here' : ''}"
          data-act="recent-chat" data-id="${escape(c.id)}" data-project="${escape(c.project)}"
          title="${escape(`${c.title}\n${c.project_name}\n${c.when}\n${count(c.turns, 'turn')}`)}">
          <span class="r-title">${escape(c.title || '(untitled)')}</span>
          <span class="r-meta">${c.scratch ? escape(c.when) : apart(escape(c.project_name), escape(c.when))}</span></button>${
          bin('chat-delete', `data-id="${escape(c.id)}" data-project="${escape(c.project)}"
              data-title="${escape(c.title || '(untitled)')}" data-scratch="${c.scratch ? 1 : ''}"`, 'Delete')}</div>`).join('')
    : '<div class="status r-empty">No chats.</div>';
  const projects = !r ? ''
    : r.projects.length ? r.projects.map((p) => `<div class="r-row"><button class="recent${p.current ? ' here' : ''}"
        data-act="recent-project" data-path="${escape(p.root)}" title="${escape(p.root)}">
        <span class="r-title">${escape(p.name)}</span><span class="r-meta">${escape(p.display)}</span></button>${
        bin('project-forget', `data-path="${escape(p.root)}" data-name="${escape(p.name)}"`, 'Remove')}</div>`).join('')
    : '<div class="status r-empty">No projects.</div>';
  return `<div class="splitter" id="splitter-r" title="Drag to resize"></div>
    <aside class="recents" style="width:${recentsWidth()}rem">
      <div class="roster-scroll">
        <div class="r-head"><h2>RECENT CHATS</h2>
          <button class="action small" data-act="new-chat"
                  ${busy ? 'disabled' : ''}>New chat</button></div>
        ${chats}
        <div class="r-head r-projects"><h2>PROJECTS</h2>
          <button class="action small" data-act="open-project"
                  ${busy ? 'disabled' : ''}>Open project</button></div>
        ${projects}
      </div>
      ${usageView()}
    </aside>`;
}

// --- the models at work ---------------------------------------------------------

/// A bar for how much of something is used, 0 to 1. Red past nine tenths.
function meter(share, title) {
  const used = Math.min(1, Math.max(0, share || 0));
  return `<div class="u-meter${used > 0.9 ? ' full' : ''}" title="${escape(title)}">
      <span style="width:${Math.round(used * 100)}%"></span></div>`;
}

/// Dollars, as a cost is read: cents under ten, whole dollars over.
const dollars = (n) => `$${n < 10 ? n.toFixed(2) : Math.round(n).toLocaleString()}`;

/// When a provider's limit fills again, from what it wrote: a time
/// ("2026-10-10T05:00:00Z") or a length ("6m0s").
function resets(text) {
  if (!text) return '';
  const when = Date.parse(text);
  return Number.isNaN(when) ? `resets in ${text}` : `resets in ${span((when - Date.now()) / 1000)}`;
}

/// The foot of the right-hand panel: the models in this machine's memory,
/// and under them the frontier model in use -- what it has cost at list
/// prices, and how much of its provider's limits and its context is used.
function usageView() {
  const s = state.snapshot;
  const total = s.memory_total || 0;
  const local = (s.local_models || []).map((m) => `
      <div class="u-row" title="${escape(m.file)}">
        <span class="u-name">${escape(m.delegator ? 'Delegator' : expertName(m.seat))}</span>
        <span class="u-num">${bytes(m.bytes)}</span></div>
      ${total ? meter(m.bytes / total, `${bytes(m.bytes)} of ${bytes(total)} memory`) : ''}`).join('')
    || '<div class="u-none">None loaded</div>';

  // The one asked last is the one in use; before any has been asked, the
  // seats that would be.
  const asked = s.frontier || [];
  let frontier;
  if (asked.length) {
    const [now, ...earlier] = asked;
    const sent = (t) => (t.input || 0) + (t.cache_read || 0) + (t.cache_write || 0);
    const live = (s.experts || []).some((e) => e.provider && e.model === now.model
      && (e.id === s.linked || working(e.id)));
    const cost = now.session.cost === undefined ? '<span class="u-num" title="Not on the price list">no price</span>'
      : `<span class="u-num" title="At list prices. This month: ${dollars(now.month.cost)}">${dollars(now.session.cost)}</span>`;
    const bars = (now.limits || []).map((l) => `<div class="u-row u-small"><span>${escape(l.what)}</span>
        <span>${compact(l.remaining)} of ${compact(l.limit)} left</span></div>
      ${meter((l.limit - l.remaining) / l.limit, resets(l.resets))}`).join('');
    const context = now.context ? `<div class="u-row u-small"><span>context</span>
        <span>${Math.round((100 * now.prompt) / now.context)}%</span></div>
      ${meter(now.prompt / now.context, `${compact(now.prompt)} of ${compact(now.context)} tokens`)}` : '';
    frontier = `<div class="u-row seat" data-phase="${live ? 'active' : 'dormant'}">
        <span class="dot"></span><span class="u-name" title="${escape(now.model)}">${escape(now.model)}</span>${cost}</div>
      <div class="u-meta">${apart(escape(now.provider), `${compact(sent(now.session))} in`, `${compact(now.session.output)} out`)}</div>
      ${now.month.cost === undefined ? '' : `<div class="u-row u-small"><span>this month</span>
        <span>${dollars(now.month.cost)}</span></div>`}
      ${bars}${context}
      ${earlier.map((m) => `<div class="u-row u-small u-other"><span class="u-name" title="${escape(m.provider)}">${escape(m.model)}</span>
        <span>${m.session.cost === undefined ? `${compact(sent(m.session) + m.session.output)} tokens` : dollars(m.session.cost)}</span></div>`).join('')}`;
  } else {
    const seats = (s.experts || []).filter((e) => e.provider);
    frontier = seats.map((e) => `<div class="u-row" title="${escape(e.name)}">
        <span class="u-name">${escape(e.model)}</span><span class="u-num">not asked yet</span></div>`).join('')
      || `<div class="u-none">${apart('None added', '<button class="link" data-act="settings-page" data-page="providers">add one</button>')}</div>`;
  }
  return `<div class="usage">
      <h2>LOCAL</h2>${local}
      <h2 class="u-frontier">FRONTIER</h2>${frontier}
    </div>`;
}

actions['fold-right'] = () =>
  setRecents(recentsOpen() ? 0 : Math.max(remember.get('recents-open', 16), SIDEBAR_MIN));

actions['recent-chat'] = (row) => guard(async () => {
  await call('history.open', { id: row.dataset.id, project: row.dataset.project });
  state.follow = true;
  if (state.view !== 'chat') enter('chat');
  need('recents', 'recents', true);
});
actions['recent-project'] = (row) => guard(async () => {
  await call('project.open', { path: row.dataset.path });
  need('recents', 'recents', true);
});
actions['chat-delete'] = async (button) => {
  const { id, project, title, scratch } = button.dataset;
  const sure = await confirmIt({ title: `Delete "${title}"?`,
    body: scratch ? 'The chat and its files. No undo.' : 'No undo.', yes: 'Delete', no: 'Cancel' });
  if (!sure) return;
  await guard(() => call('session.delete', { id, project }));
  need('recents', 'recents', true);
};
actions['project-forget'] = async (button) => {
  const sure = await confirmIt({ title: `Remove ${button.dataset.name}?`,
    body: 'From this list only. The folder stays.', yes: 'Remove', no: 'Cancel' });
  if (!sure) return;
  await guard(() => call('project.forget', { path: button.dataset.path }));
  need('recents', 'recents', true);
};
actions['new-chat'] = () => guard(async () => {
  await call('session.new');
  if (state.view !== 'chat') enter('chat');
  need('recents', 'recents', true);
});

/// Kept current without asking: when the project, the conversation or its
/// length changes, and the engine is not mid-turn, the list is read again.
let recentsKey = '';
let recentsTimer = 0;
afterDraw.push(() => {
  if (!recentsOpen()) return;
  const s = state.snapshot;
  const key = `${(s.project || {}).root}|${s.session}|${s.session_name}|${(s.turns || []).length}|${!!s.busy}`;
  if (key === recentsKey || s.busy) return;
  recentsKey = key;
  clearTimeout(recentsTimer);
  recentsTimer = setTimeout(() => need('recents', 'recents', true), 300);
});

/// What a seat row says when the pointer rests on it.
function seatTip(e) {
  const where = e.provider ? `${e.model}, answered by ${e.provider}`
              : e.model ? e.model + (e.phase === 'missing' ? '  (missing)' : '')
              : 'no model yet';
  return `${e.blurb ? e.blurb + '\n' : ''}${where}${
    e.made ? '\nMade by a build for work nobody on the roster fitted' : ''}`;
}

/// The task an expert has in the build that is running, when it has one --
/// what the side menu lights the seat for, and where clicking it goes.
function working(id) {
  const cook = state.snapshot.cook;
  if (!cook || !cook.running) return null;
  return (cook.tasks || []).find((t) => t.expert === id && t.state === 'working') || null;
}

/// Click a seat: the work it is doing, when it is doing some -- the build, on
/// that agent, with the file it touched last open -- and its settings when
/// it is not.
actions['seat-open'] = (button) => {
  const id = button.dataset.expert;
  const cook = state.snapshot.cook;
  const tasks = cook ? (cook.tasks || []) : [];
  const task = working(id) || (cook && cook.kind === 'build'
    ? tasks.filter((t) => t.expert === id && t.state !== 'waiting').slice(-1)[0] : null);
  if (task) {
    // The pane is remembered, and entering Build reads it back.
    remember.set('build-pane', 'agents');
    state.open.task = task.index;
    enter('build');
    return;
  }
  enter('settings', 'experts');
};

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
  const problem = s.delegator_problem || '';
  const delegatorPhase = routing ? 'active' : problem ? 'missing'
                       : delegator.model ? 'dormant' : 'unconfigured';

  if (!sidebarOpen()) {
    // Closed, it is a rail rather than nothing: the dots still say which seat
    // is loaded, which is the one thing worth a glance while it is folded.
    const dots = experts.map((e, i) =>
      `<button class="seat" data-act="seat-open" data-expert="${escape(e.id)}"
               data-phase="${i === linked || working(e.id) ? 'active' : e.phase}" title="${escape(e.name)}">
         <span class="dot"></span></button>`).join('');
    // And under them a dot for each agent at work, which opens its work.
    const cook = state.snapshot.cook;
    const busy = cook && cook.running ? (cook.tasks || []).filter((t) => t.state === 'working') : [];
    const agentDots = busy.map((t) =>
      `<button class="seat" data-act="agent-open" data-index="${t.index}" data-phase="active"
               title="${escape(`${t.index + 1}. ${t.title}`)}"><span class="dot"></span></button>`).join('');
    return `<aside class="rail">
        <button class="seat" data-act="settings-page" data-page="general"
                data-phase="${delegatorPhase}"
                title="${escape(delegator.model || 'no delegator')}"><span class="dot"></span></button>
        <div class="rail-gap"></div>${dots}${agentDots ? `<div class="rail-gap"></div>${agentDots}` : ''}
      </aside>`;
  }

  // The line from the delegator runs down the list to the seat with the
  // turn, so a seat's place on it is its index in the roster.
  const seatRow = (e, i) => {
    const classes = ['seat'];
    if (linked >= 0 && i < linked) classes.push('on-trunk');
    if (i === linked) classes.push('elbow', 'linked');
    const busy = working(e.id);
    return `<button class="${classes.join(' ')}" data-act="seat-open" data-expert="${escape(e.id)}"
        data-phase="${(i === linked || busy) && e.phase !== 'loading' ? 'active' : e.phase}"
        title="${escape(busy ? e.name + ' is working on task ' + (busy.index + 1) + ': ' + busy.title + ' -- click to see it'
                             : seatTip(e))}">
      <span class="dot"></span><span class="name">${escape(e.name)}</span>
      ${e.provider ? `<span class="cloud" title="Answered by ${escape(e.provider)}">${ICONS.cloud}</span>` : ''}
      ${e.phase === 'loading' ? ring(e.progress) : ''}
    </button>`;
  };
  // Every seat on the roster, those a build made for work nobody fitted
  // among them: they are experts like any other once they are made.
  const seats = experts.map(seatRow).join('') || '<div class="status">None yet.</div>';
  // Under the experts, the agents of the build: the architect and one per
  // task, each saying whose it is and how far it has got, and each opening
  // its work. Here rather than only in the Build view, so what is at work
  // is in sight from whichever view is open.
  const build = state.snapshot.cook;
  const agents = build ? `<h2 class="experts-head agents-head"
        title="The build's agents: the architect, and one for each task. Click one to see its work.">AGENTS</h2>
      ${agentRows(build, agentShown(build))}` : '';

  // What the program last said stays on the line until something else is:
  // an idle engine says nothing, which would otherwise wipe it the moment it
  // was put there -- a crash pointed at on startup was never seen.
  const said = (s.notices || []).slice(-6).join('\n');
  const status = state.error || s.status || (s.notices || []).slice(-1)[0] || '';
  const loaded = !!s.resident || (s.delegator_ready && !!delegator.model);
  // Eject is sized to its own word rather than to the panel: it is an
  // occasional action, and a button stretched across a side menu that can be
  // dragged to four hundred pixels reads as the most important thing here.
  return `<aside style="width:${sidebarWidth()}rem">
      <div class="side-status${state.error ? ' bad' : s.busy ? ' busy' : ''}" title="${escape(said || status)}">${
        escape(status) || '&nbsp;'}</div>
      <div class="roster-scroll">
        <h2${linked >= 0 ? ' class="on-trunk-gap"' : ''}>DELEGATOR</h2>
        <button class="seat${routing ? ' linked' : ''}${linked >= 0 ? ' trunk-start' : ''}"
                data-act="settings-page" data-page="general" data-phase="${delegatorPhase}"
                title="${escape(problem
                  ? `${delegator.model}: ${problem}\nRouting on keywords`
                  : delegator.model ? delegator.model
                  : 'No delegator. Routing on keywords')}">
          <span class="dot"></span><span class="name">${escape(delegator.model || '(none)')}</span>
          ${loading ? ring(s.delegator_progress) : ''}
        </button>
        <h2 class="experts-head${linked >= 0 ? ' on-trunk' : ''}">EXPERTS</h2>${seats}
        <button class="action wide" data-act="settings-page" data-page="experts">Manage experts</button>
        ${agents}
      </div>
      <div class="roster-foot">
        <button class="action" data-act="eject" ${loaded ? '' : 'disabled'}
                title="${loaded ? 'Unload every model' : 'Nothing is loaded'}">Eject</button>
      </div>
    </aside>
    <div class="splitter" id="splitter" title="Drag to resize"></div>`;
}

actions.fold = () => setSidebar(sidebarOpen() ? 0 : Math.max(remember.get('sidebar-open', 15), SIDEBAR_MIN));
actions.eject = () => guard(() => call('release', { all: true }));

/// Dragging the edge of the side menu. Bound once to the document rather
/// than to the splitter, which is redrawn while it is being dragged.
function listenForSplitters() {
  document.addEventListener('pointerdown', (down) => {
    const handle = down.target.closest && down.target.closest('#splitter, #splitter-r, #composer-splitter');
    if (!handle) return;
    down.preventDefault();
    const rem = parseFloat(getComputedStyle(document.documentElement).fontSize) || 16;
    if (handle.id === 'splitter-r') return dragRecents(down, rem);
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

/// The right-hand panel's edge, which widens it when dragged left.
function dragRecents(down, rem) {
  const from = down.clientX;
  const start = recentsOpen() ? recentsWidth() : 0;
  document.body.classList.add('dragging-x');
  const move = (event) => setRecents(start + (from - event.clientX) / rem);
  const up = () => {
    document.removeEventListener('pointermove', move);
    document.removeEventListener('pointerup', up);
    document.body.classList.remove('dragging-x');
    if (state.recentsWidth < SIDEBAR_SHUT) setRecents(0);
    else {
      if (state.recentsWidth < SIDEBAR_MIN) setRecents(SIDEBAR_MIN);
      remember.set('recents-open', state.recentsWidth);
    }
  };
  document.addEventListener('pointermove', move);
  document.addEventListener('pointerup', up);
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
  const tab = { 1: 'chat', 2: 'build', 3: 'create', 4: 'history' }[event.key];
  if (tab) { event.preventDefault(); enter(tab); }
  if (event.key === ',') { event.preventDefault(); actions.gear(); }
  // Attach, from wherever the box is. Not over a dialog: the box is not
  // what that is about.
  if ((event.key === 'u' || event.key === 'U') && !state.modal
      && (state.view === 'chat' || state.view === 'build')) {
    event.preventDefault();
    attachPick(false);
  }
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
  const project = state.snapshot.project || {};
  const asking = project.pending_trust;
  const showing = state.modal && state.modal.kind === 'trust';
  if (asking && !showing) {
    openModal({ kind: 'trust', path: project.pending_display || asking, sticky: true });
  }
  else if (!asking && showing) closeModal();
}

modals.trust = (m) => `<div class="modal">
    <div class="head"><strong>Trust this folder?</strong></div>
    <div class="body-pad">
      <div class="crumbs">${escape(m.path)}</div>
      <div class="status">Experts can read, write and run commands here.</div>
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
    <div class="head"><strong>New expert</strong></div>
    <div class="body-pad">
      <div class="field"><label for="ne-name">Name</label>
        <input id="ne-name" data-input="ne-field" data-field="name" data-draft data-focus
               placeholder="Rust Async, Tax Law, Kubernetes" value="${escape(m.name)}"></div>
      <div class="field"><label for="ne-what">Description</label>
        <textarea id="ne-what" rows="4" data-input="ne-field" data-field="description" data-draft>${
          escape(m.description)}</textarea>
        <div class="hint">Used for routing.</div></div>
      <div class="field"><label for="ne-model">Model</label>
        ${modelSelect(m.model, m.provider, { id: 'ne-model', 'data-change': 'ne-model' })}
        <div class="hint">Optional.</div></div>
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

/// When Crucible finishes fetching something for itself, what the page knew
/// about runtimes is out of date: the empty chat asked at startup, before
/// there were any, and would go on saying "No runtime" over a working one.
let setupFinished = '';
onSnapshot.push(() => {
  const items = (state.snapshot.setup && state.snapshot.setup.items) || [];
  const finished = items.filter((i) => i.state === 'done' || i.state === 'failed')
    .map((i) => i.id).join(',');
  if (finished !== setupFinished) {
    setupFinished = finished;
    if (finished) {
      need('runtimes', 'runtimes', true);
      need('trainer', 'trainer', true);
    }
  }
});

/// The window's remembered shape, from wherever it is remembered.
function recall() {
  state.sidebar = remember.get('sidebar', 15);
  state.composerRows = remember.get('composer-rows', 1);
  state.route = remember.get('route', '');
  state.recentsWidth = remember.get('recents-width', 16);
}

function start() {
  recall();
  state.mark = document.body.getAttribute('data-mark') || '';
  listen();
  listenForSplitters();
  guard(async () => {
    // The config comes down with the first snapshot rather than when the
    // settings screen is first opened: the chat view reads it to decide what
    // to say when there is nothing to chat with. The window's remembered
    // shape with them, so the first real draw is already the right one.
    const [snapshot, config, prefs] = await Promise.all([
      call('snapshot'), call('config'), call('prefs').catch(() => ({}))]);
    remember.saved = prefs && typeof prefs === 'object' ? prefs : {};
    recall();
    state.snapshot = snapshot;
    state.config = config;
    trustWatch();
  });
  // What the empty chat needs to know before it can say what is missing.
  need('runtimes', 'runtimes');
}
