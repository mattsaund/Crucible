// SPDX-License-Identifier: MIT
//
// The shelf: the project's code, its version control and a preview of what
// it shows, on the right of the conversation.
//
// A person watching an expert work on a project wants to read what it wrote,
// see it and keep it, not only watch a transcript of it being made. The
// editor is code.js and the preview preview.js; which pane is up, the shelf,
// the version control, and keeping what each has read in step with the work
// are here.

const CHAT_PANES = [
  ['chat',    'Chat',    'The conversation'],
  ['code',    'Code',    'The project\'s files'],
  ['source',  'Source',  'Version control'],
  ['preview', 'Preview', 'What it shows'],
];

// 'chat' is the shelf collapsed.
state.chatPane = 'chat';
// The pane it opens on.
state.shelfTab = 'code';
// How much of the width it takes, in percent. Remembered with the window's
// shape: see recall.
state.shelfShare = 50;
state.tree = null;
state.source = null;
state.diff = null;
state.log = null;
state.ship = { command: '', tag: '', notes: '', output: '', running: false };

// --- the panes ----------------------------------------------------------------------

/// Whether there is anything to show on the shelf: a folder with something in
/// it. A project's is taken to have, until it has been read.
function folderShown() {
  const project = state.snapshot.project || {};
  if (!project.open) return false;
  if (!state.tree) return !project.scratchpad;
  return !!(state.tree.entries && state.tree.entries.length);
}

/// The pane on screen: the one chosen, while there is a folder to show.
function shownPane() {
  return folderShown() ? state.chatPane : 'chat';
}

/// Above the conversation, its panes. The conversation's tab says when it
/// wants somebody: an edit waiting on a yes.
function paneNav() {
  if (!folderShown()) return '';
  const wanted = !!state.snapshot.pending_edit;
  const here = shownPane();
  return `<div class="pane-nav">${CHAT_PANES.map(([id, label, tip]) =>
    `<button data-act="chat-pane" data-pane="${id}" aria-current="${here === id}" title="${tip}">${label}${
      id === 'chat' && wanted && here !== 'chat' ? '<span class="pip"></span>' : ''}</button>`).join('')}</div>`;
}

function setPane(pane) {
  if (pane === 'chat') shelfOpenedFor = promptKey();
  else state.shelfTab = pane;
  state.chatPane = pane;
  render();
  enterPane(pane);
}

actions['chat-pane'] = (button) => setPane(button.dataset.pane);

// --- the shelf ---------------------------------------------------------------------
//
// Code, Source and Preview, on the right of the conversation. It starts
// collapsed, with nothing of it on screen and the button in the top bar to
// open it, and expands by itself when a model makes something to show (see
// shelfFollows). With nothing to show it says so and no more.

/// The narrowest the chat area can be and hold the conversation and the shelf
/// side by side, in rem. Under it the shelf takes the whole area and its Chat
/// tab is the way back. The @container rule in style.css says the same.
const SHELF_ROOM = 52;

/// As little of the width as either side can be dragged to, in percent.
const SHELF_LEAST = 25;

const shelfOpen = () => state.chatPane !== 'chat';

function shelfView() {
  if (!shelfOpen()) return '';
  const pane = shownPane();
  return `<div class="splitter" id="splitter-shelf" data-act="shelf-edge" title="Drag to resize"></div>
    <section class="shelf" aria-label="Shelf">
      ${pane === 'chat' ? '<div class="shelf-empty">Nothing</div>'
                        : `<div class="shelf-head">${paneNav()}</div>${shelfBody(pane)}`}
    </section>`;
}

/// What the shelf shows, made again only when it may have changed. Coloring
/// a long file or parsing a page into its frame for every token of a reply
/// streaming in beside it is what would make the two together slow.
let shelfMade = { pane: '', html: '' };
function shelfBody(pane) {
  if (!shelfKept || shelfMade.pane !== pane) {
    const body = pane === 'code' ? codePane() : pane === 'source' ? sourcePane() : previewView();
    shelfMade = { pane, html: `<div id="pane-body">${body}</div>` };
  }
  return region('pane-body', shelfMade.html);
}

/// What the shelf reads from a push. One that leaves this as it was changed
/// the conversation and nothing on the shelf.
const shelfInputs = (s) => `${!!s.busy}|${s.platform || ''}|${JSON.stringify(s.project || {})}`;

/// Dragging the edge between the conversation and the shelf. Written straight
/// onto the layout rather than through a draw, so the edge keeps up with the
/// pointer however much is on screen.
function dragShelf(handle) {
  const split = handle.parentElement;
  const box = split.getBoundingClientRect();
  document.body.classList.add('dragging-x');
  const move = (event) => {
    const share = ((box.right - event.clientX) / box.width) * 100;
    state.shelfShare = Math.round(Math.max(SHELF_LEAST, Math.min(100 - SHELF_LEAST, share)) * 10) / 10;
    split.style.setProperty('--shelf', `${state.shelfShare}%`);
  };
  const up = () => {
    document.removeEventListener('pointermove', move);
    document.removeEventListener('pointerup', up);
    document.body.classList.remove('dragging-x');
    remember.set('shelf-share', state.shelfShare);
    render();
  };
  document.addEventListener('pointermove', move);
  document.addEventListener('pointerup', up);
}

actions['shelf-toggle'] = () => setPane(shelfOpen() ? 'chat' : state.shelfTab);

/// Pressed twice, the edge goes back to the middle.
actions['shelf-edge'] = (edge, event) => {
  if (event.detail < 2) return;
  state.shelfShare = 50;
  remember.set('shelf-share', 50);
  render();
};

/// Which prompt this is: the folder it works in and where it is in the chat.
const promptKey = () => `${(state.snapshot.project || {}).root || ''}|${(state.snapshot.turns || []).length}`;

/// The prompt the shelf last opened for, or was collapsed during.
let shelfOpenedFor = '';

/// Whether the conversation and the shelf would fit side by side.
function shelfFits() {
  const pane = document.querySelector('.chat-pane');
  if (!pane) return true;
  const rem = parseFloat(getComputedStyle(document.documentElement).fontSize) || 16;
  return pane.clientWidth >= SHELF_ROOM * rem;
}

/// The first file a prompt writes expands the shelf on it: a page as the
/// page, anything else as its code. Once a prompt, and the caret stays in the
/// box. Collapsed during a prompt, it stays collapsed for the rest of it.
function shelfFollows(written) {
  const key = promptKey();
  if (!written.length || shelfOpenedFor === key) return;
  shelfOpenedFor = key;
  if (state.view !== 'chat' || shownPane() !== 'chat' || !shelfFits()) return;
  const page = written.find((file) => /\.html?$/i.test(file));
  if (page) {
    state.chatPane = state.shelfTab = 'preview';
    state.preview.url = '';
    loadPreview(page);
    enterPreview();
  } else if (state.preview.page && written.some((file) => /\.(css|js|svg)$/i.test(file))) {
    state.chatPane = state.shelfTab = 'preview';
    refreshPreview();
  } else {
    openFile(written[0], 0, true);
  }
}

/// What a pane reads as it opens. The tree every time: a turn may have
/// changed it while another pane was up.
function enterPane(pane) {
  if (pane === 'code') {
    loadTree();
    refreshTabs();
  }
  if (pane === 'source' && !state.source) loadSource();
  if (pane === 'preview' && !state.preview.pages) enterPreview();
}

entering.chat = () => enterPane(shownPane());

// --- version control --------------------------------------------------------------------

function sourcePane() {
  const project = state.snapshot.project || {};
  if (!project.open) return '<div class="empty"><div class="empty-label">No project open.</div></div>';
  const s = state.source;
  if (!s) return '<div class="status" style="padding:1rem">Reading...</div>';
  if (!s.git) return '<div class="empty"><div class="empty-label">git is not installed</div></div>';
  const busy = !!state.snapshot.busy || state.ship.running;
  if (!s.repo) {
    return `<div class="empty"><div class="empty-label quiet">Not a repository yet</div>
      <button class="action" data-act="git-init" ${busy ? 'disabled' : ''}>Start one here</button></div>`;
  }
  const changes = s.changes.length ? s.changes.map((c) => `<button class="tree-row${state.open.diff === c.path ? ' here' : ''}"
        data-act="diff-open" data-path="${escape(c.path)}" title="${escape(c.path)}">
      <span class="git-status">${escape(c.status)}</span>${escape(c.path)}</button>`).join('')
    : '<div class="status" style="padding:.3rem .6rem">Nothing changed since the last commit.</div>';
  const diff = state.diff === null ? '' : state.diff === undefined ? '<div class="status">Reading...</div>'
    : state.diff ? codeBlock(state.diff, 'diff') : '<div class="status">No difference.</div>';
  const log = !state.log ? '' : state.log.length ? state.log.map((c) => `<div class="commit">
      <code>${escape(c.hash)}</code> ${escape(c.subject)} <span class="status">${apart(escape(c.author), escape(c.when))}</span></div>`).join('')
    : '<div class="status">No commits yet.</div>';
  const ship = state.ship;
  return `<div class="source">
      <div class="row source-head"><strong>${escape(s.branch || '(no branch)')}</strong>
        <span class="status">${apart(s.remote ? escape(s.remote) : 'no remote', s.ahead ? `${s.ahead} ahead` : '',
          s.behind ? `${s.behind} behind` : '')}</span>
        <span class="spacer"></span>
        <button class="action small" data-act="source-refresh">Refresh</button>
        ${s.remote ? `<button class="action small" data-act="git-pull" ${busy ? 'disabled' : ''}>Pull</button>
                      <button class="action small" data-act="git-push" ${busy ? 'disabled' : ''}>Push</button>`
                   : s.gh ? `<button class="action small" data-act="git-publish" ${busy ? 'disabled' : ''}>Publish to GitHub</button>`
                   : '<span class="hint" title="Needs gh">no gh</span>'}</div>
      <div class="caption" style="margin-top:1rem">CHANGES</div>
      <div class="changes-list">${changes}</div>
      <form class="row" data-submit="git-commit" style="margin-top:.6rem">
        <input id="commit-message" data-draft placeholder="Commit message" aria-label="Commit message" autocomplete="off">
        <button class="action" ${busy || !s.changes.length ? 'disabled' : ''}>Commit</button></form>
      ${diff}
      <div class="caption" style="margin-top:1.2rem">HISTORY</div>${log}
      ${ciView()}
      <form class="row" data-submit="git-command" style="margin-top:1rem">
        <span class="status">git</span>
        <input id="git-args" data-draft placeholder="any git command: log --oneline -5, branch, stash..." aria-label="git arguments"
               autocomplete="off" spellcheck="false">
        <button class="action" ${busy ? 'disabled' : ''}>Run</button></form>
      ${state.gitOutput ? codeBlock(state.gitOutput, 'shell') : ''}
      <div class="caption" style="margin-top:1.4rem">SHIP</div>
      <div class="hint">Package, then release${s.gh ? ' on GitHub' : ' (needs gh)'}.</div>
      <form class="row" data-submit="ship-package" style="margin-top:.5rem">
        <input id="ship-command" data-draft value="${escape(ship.command || '')}"
               placeholder="the command that packages it: npm run build, cargo build --release..." aria-label="Package command"
               autocomplete="off" spellcheck="false">
        <button class="action" ${busy ? 'disabled' : ''}>${ship.running ? 'Running...' : 'Package'}</button></form>
      ${ship.output ? codeBlock(ship.output, 'shell') : ''}
      <form class="row" data-submit="ship-release" style="margin-top:.5rem">
        <input id="ship-tag" data-draft value="${escape(ship.tag)}" placeholder="v1.0.0" aria-label="Release tag" style="flex:0 0 9rem"
               autocomplete="off" spellcheck="false">
        <input id="ship-notes" data-draft value="${escape(ship.notes)}" placeholder="release notes" aria-label="Release notes" autocomplete="off">
        <button class="action" ${busy || !s.gh ? 'disabled' : ''}>Release</button></form>
      ${ship.url ? `<div class="hint">Released: ${escape(ship.url)}</div>` : ''}
      <div class="row" style="margin-top:.8rem">
        <button class="action small" data-act="chat-pane" data-pane="preview">Make it an app</button></div>
    </div>`;
}

/// The latest CI runs on GitHub: what they checked and whether it passed.
function ciView() {
  const ci = state.ci;
  if (!ci || !ci.available) return '';
  const mark = (run) => run.status !== 'completed' ? '◐ running'
    : run.conclusion === 'success' ? '✓ passed' : run.conclusion === 'skipped' ? '– skipped' : '✗ ' + (run.conclusion || 'failed');
  const rows = (ci.runs || []).map((run) => `<div class="commit ci-run" data-conclusion="${escape(run.conclusion || run.status || '')}">
      <span class="ci-mark">${escape(mark(run))}</span> ${escape(run.name || '')}
      <span class="status">${apart(escape(run.displayTitle || ''), escape(run.headBranch || ''), escape(run.event || ''))}</span></div>`);
  return `<div class="caption" style="margin-top:1.2rem">CI ON GITHUB</div>${
    rows.length ? rows.join('') : '<div class="status">No runs yet.</div>'}`;
}

/// What git says of the project. CI is asked of GitHub, which takes its
/// time, only when the pane is opened or refreshed: not after every action
/// of a turn. One look at a time.
let sourceLoading = false;
async function loadSource(withCi = true) {
  if (!(state.snapshot.project || {}).open || sourceLoading) return;
  sourceLoading = true;
  try {
    state.source = await call('git.status');
    if (state.source.repo) state.log = (await call('git.log')).commits; else state.log = null;
    // CI only where there is some to ask about: a GitHub remote, and gh.
    const hasCi = state.source.gh && /github\.com/.test(state.source.remote || '');
    if (!hasCi) state.ci = null;
    else if (withCi) state.ci = await call('git.ci');
  } catch (error) { state.error = error.message; }
  sourceLoading = false;
  render();
}

actions['source-refresh'] = () => { state.source = null; state.diff = null; render(); loadSource(); };
actions['git-init'] = () => guard(async () => { await call('git.init'); state.source = null; loadSource(); });
actions['diff-open'] = (button) => guard(async () => {
  state.open.diff = button.dataset.path;
  state.diff = undefined;
  render();
  const got = await call('git.diff', { path: button.dataset.path });
  state.diff = got.diff || '';
});
actions['git-commit'] = (form) => guard(async () => {
  const box = form.querySelector('input');
  const message = box.value.trim();
  if (!message) throw new Error('A commit needs a message.');
  const got = await call('git.commit', { message });
  box.value = '';
  state.error = '';
  state.snapshot.status = got.summary;
  state.diff = null;
  state.source = null;
  loadSource();
});
actions['git-push'] = () => guard(async () => { await call('git.push'); state.snapshot.status = 'pushed'; state.source = null; loadSource(); });
actions['git-pull'] = () => guard(async () => { await call('git.pull'); state.snapshot.status = 'pulled'; state.source = null; loadSource(); });
actions['git-publish'] = async () => {
  const project = state.snapshot.project || {};
  const sure = await confirmIt({ title: `Publish ${project.name} to GitHub?`,
    body: 'Private, under your account.',
    yes: 'Publish', no: 'Cancel' });
  if (!sure) return;
  await guard(async () => {
    const got = await call('git.publish', { name: project.name, private: true });
    state.snapshot.status = got.url ? `published at ${got.url}` : 'published';
    state.source = null;
    loadSource();
  });
};
actions['git-command'] = (form) => guard(async () => {
  const box = form.querySelector('input');
  const args = box.value.trim();
  if (!args) return;
  const got = await call('git.run', { args });
  state.gitOutput = `$ git ${args}\n${got.output}${got.ok ? '' : `\n(exit status ${got.status})`}`;
  state.source = null;
  loadSource();
});
actions['ship-package'] = (form) => guard(async () => {
  const command = form.querySelector('input').value.trim();
  if (!command) throw new Error('Say what packages it.');
  state.ship.command = command;
  state.ship.running = true;
  state.ship.output = '';
  render();
  try {
    const got = await call('project.run', { command, timeout: 1800 });
    state.ship.output = `${got.summary}\n${got.output}`;
  } finally { state.ship.running = false; }
});
actions['ship-release'] = (form) => guard(async () => {
  const inputs = form.querySelectorAll('input');
  state.ship.tag = inputs[0].value.trim();
  state.ship.notes = inputs[1].value.trim();
  if (!state.ship.tag) throw new Error('A release needs a tag, like v1.0.0.');
  const got = await call('git.release', { tag: state.ship.tag, notes: state.ship.notes });
  state.ship.url = got.url || '';
  state.snapshot.status = `released ${state.ship.tag}`;
});

// --- keeping what the panes read in step --------------------------------------------------

/// Another conversation starts with the shelf collapsed. A chat that had no
/// id and gets one is the same conversation.
let shelfSession = '';
onSnapshot.push(() => {
  const session = state.snapshot.session || '';
  if (session === shelfSession) return;
  const was = shelfSession;
  shelfSession = session;
  if (was) state.chatPane = 'chat';
});

/// A project change empties what was read of the old one, and the pane on
/// screen reads the new one: the preview's page list among them, which
/// preview.js has emptied by now, since it loads before this file. A chat's
/// scratch folder is looked at at once: its panes show once it has
/// something in it.
let chatProject = '';
onSnapshot.push(() => {
  const project = state.snapshot.project || {};
  const root = project.root || '';
  if (root === chatProject) return;
  chatProject = root;
  state.tree = null;
  state.source = null;
  state.diff = null;
  state.log = null;
  state.open.file = null;
  state.code.tabs = [];
  state.code.search.results = null;
  if (project.scratchpad) loadTree();
  if (state.view === 'chat') enterPane(shownPane());
});

/// What a turn leaves in an empty folder is what fills the shelf: looked at
/// again whenever a turn ends, until something is there.
let turnsSeen = '';
onSnapshot.push(() => {
  const s = state.snapshot;
  const project = s.project || {};
  const key = `${project.root}|${(s.turns || []).length}|${!!s.busy}`;
  if (key === turnsSeen) return;
  turnsSeen = key;
  if (project.open && !s.busy && !folderShown()) loadTree();
});

/// An action a turn takes -- a file written, a command run -- may leave the
/// tree and the version control stale, and may have rewritten the file on
/// show or the page in the preview. The pane on screen is read again as each
/// action lands; the others when they open.
let actionsSeen = { root: null, turns: 0, actions: 0 };
onSnapshot.push(() => {
  const turns = state.snapshot.turns || [];
  const last = turns[turns.length - 1];
  const done = last ? last.actions || [] : [];
  const root = (state.snapshot.project || {}).root || '';
  const was = actionsSeen;
  actionsSeen = { root, turns: turns.length, actions: done.length };
  // Only actions added to the same turn of the same conversation: a turn
  // opened, deleted or reopened from History changed nothing on disk.
  const added = was.root === root && was.turns === turns.length ? done.length - was.actions : 0;
  if (added <= 0) return;
  // A write or an edit names its file as the language its body is drawn in.
  const written = done.slice(-added).map((a) => a.language || '').filter(Boolean);
  const touchedPage = written.some((f) => /\.(html?|css|js|svg|png|jpe?g|gif)$/i.test(f));
  codeFilesChanged(written);
  // What was up before the shelf may open for this: what it opens on it
  // reads for itself.
  const pane = state.view === 'chat' ? shownPane() : '';
  shelfFollows(written);
  if (pane === 'code' || (written.length && !folderShown())) loadTree();
  if (pane === 'source') loadSource(false);
  else state.source = null;   // read again when its pane opens
  if (pane === 'preview' && touchedPage) refreshPreview();
});

/// The page again, unless the person is in the middle of tuning it: then it
/// waits, and the strip under the bar says it has changed.
function refreshPreview() {
  const p = state.preview;
  if (p.url) return;
  if (p.changes.length || p.picked || p.making) { p.stale = true; return; }
  if (p.page) loadPreview(p.page); else enterPreview();
}
