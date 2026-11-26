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
  ['chat',     'Chat',     'The conversation'],
  ['code',     'Code',     'The project\'s files'],
  ['source',   'Source',   'Version control'],
  ['preview',  'Preview',  'What it shows'],
  ['terminal', 'Terminal', 'A shell in the project'],
];

// 'chat' is the shelf collapsed, 'shelf' its list of what the experts made,
// and the others the build entered from it.
state.chatPane = 'chat';
// The shelf's list, as `shelf` answers it, the download or save last made
// and the one under way.
state.shelf = null;
state.shelfSaved = null;
state.shelfSaving = '';
// The thing on it in the viewer, or null. See viewerView.
state.viewer = null;
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

/// The pane on screen: the list, one thing on it in the viewer, or the one
/// chosen in a build while there is a folder to show.
function shownPane() {
  if (state.chatPane === 'shelf') return 'shelf';
  if (state.chatPane === 'view') return state.viewer ? 'view' : 'shelf';
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
  else if (pane !== 'shelf' && pane !== 'view') state.shelfTab = pane;
  if (pane !== 'view') closeViewer();
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
      ${pane === 'shelf' || pane === 'chat' ? shelfList() : pane === 'view' ? viewerView()
        : `<div class="shelf-head">${paneNav()}
             <button class="action small back" data-act="back-to-shelf">Back to shelf</button></div>${shelfBody(pane)}`}
    </section>`;
}

// --- the list -------------------------------------------------------------------------
//
// What the experts made in this conversation, kept with it: its build, which
// is entered to read, edit and preview the code, and every other thing,
// each to download. See session/shelf.hpp.

/// What each kind of download is called on its card.
const SHELF_KINDS = {
  pdf: 'PDF', document: 'Document', sheet: 'Spreadsheet', slides: 'Slides', image: 'Picture',
  audio: 'Recording', video: 'Video', archive: 'Archive', text: 'Text', file: 'File',
};

/// The list. In a window too narrow for both, the Chat tab is the way back.
function shelfList() {
  const listing = state.shelf;
  const head = `<div class="shelf-head"><div class="pane-nav">
      <button data-act="chat-pane" data-pane="chat" aria-current="false" title="The conversation">Chat</button></div>
      <span class="shelf-title">Shelf</span></div>`;
  if (!listing) return `${head}<div class="shelf-empty">Reading...</div>`;
  const build = listing.build;
  const items = listing.items || [];
  if (!build && !items.length) return `${head}<div class="shelf-empty">Nothing</div>`;
  const card = build ? `<div class="shelf-card build">
      <div class="shelf-kind">Build</div>
      ${build.name ? `<div class="shelf-name">${escape(build.name)}</div>` : ''}
      <div class="shelf-meta">${apart(build.files ? `${build.files} file${build.files === 1 ? '' : 's'}` : '',
                                      build.updated ? ago(build.updated) : '')}</div>
      <div class="shelf-acts"><button class="action" data-act="build-enter">Enter build</button>
        ${keepButtons(BUILD)}
        ${build.project ? '' : '<button class="link" data-act="build-remove">Delete</button>'}</div>
      ${savedLine(BUILD)}</div>` : '';
  const rest = items.map((item) => `<div class="shelf-card" title="${escape(item.path)}">
      <div class="shelf-kind">${escape(SHELF_KINDS[item.kind] || 'File')}</div>
      <button class="shelf-name link" data-act="shelf-view" data-path="${escape(item.path)}">${escape(item.name)}</button>
      <div class="shelf-meta">${apart(bytes(item.bytes), ago(item.added))}</div>
      <div class="shelf-acts"><button class="action" data-act="shelf-view" data-path="${escape(item.path)}">View</button>
        ${keepButtons(item.path)}
        <button class="link" data-act="shelf-delete" data-path="${escape(item.path)}" data-name="${escape(item.name)}">Delete</button></div>
      ${savedLine(item.path)}
    </div>`).join('');
  return `${head}<div class="shelf-list">${card}${rest}</div>`;
}

/// What stands for the build where a thing on the shelf is named by its
/// path, which is never one: a path on the shelf is relative.
const BUILD = '/build';

/// Download, into the downloads folder as a browser would, and Save as,
/// where the platform's own dialog says. The build goes as one archive.
/// `small` for the viewer's bar.
function keepButtons(path, small) {
  const saving = state.shelfSaving === path;
  return `<button class="action${small ? ' small' : ''}" data-act="shelf-download" data-path="${escape(path)}" ${
      saving ? 'disabled' : ''}>${saving ? 'Saving...' : 'Download'}</button>
    ${window.saveAs ? `<button class="link" data-act="shelf-save" data-path="${escape(path)}" ${saving ? 'disabled' : ''}>Save as</button>` : ''}`;
}

/// Where the last download or save of `path` went.
function savedLine(path) {
  const saved = state.shelfSaved;
  if (!saved || saved.path !== path) return '';
  return `<div class="shelf-saved">${apart(`Saved to ${escape(saved.to)}`,
    saved.downloads && window.showDownloads ? '<button class="link" data-act="shelf-show-downloads">Show</button>' : '')}</div>`;
}

/// Read the list again.
function loadShelf() {
  return guard(async () => { state.shelf = await call('shelf'); });
}

// --- the viewer -------------------------------------------------------------------------
//
// One thing on the shelf, shown on the shelf: a PDF in the window's own
// reader, a picture, a recording or a film played, a document's words, a
// table as a table, what is in an archive. What the window draws itself
// comes in pieces (shelf.bytes) and is handed to it as a blob, which lasts as
// long as the viewer is on it. A PDF can be read as its words as well, which
// is what is shown where the window has no reader for one.

/// What the window draws from the bytes themselves.
const DRAWN = ['pdf', 'image', 'audio', 'video'];

/// The most of a text that is colored, or set as a table or as Markdown.
/// Past it, a text is shown plain: coloring a few megabytes holds the window.
const VIEW_COLORED = 200000;

/// The most rows of a table shown.
const VIEW_ROWS = 1000;

/// Show `path` in the viewer.
async function openViewer(path) {
  closeViewer();
  const v = state.viewer = { path, name: path.split('/').pop(), loading: true, mode: 'page', got: 0 };
  state.chatPane = 'view';
  render();
  try {
    const seen = await call('shelf.view', { path });
    if (state.viewer !== v) return;
    Object.assign(v, seen);
    if (DRAWN.includes(seen.show)) {
      if (seen.show === 'pdf' && navigator.pdfViewerEnabled === false) await viewerWords(v);
      else v.url = await blobOf(v, seen.mime);
    }
  } catch (error) {
    v.error = error.message;
  }
  // Put away while it was read: the blob goes with it.
  if (state.viewer !== v) {
    if (v.url) URL.revokeObjectURL(v.url);
    return;
  }
  v.loading = false;
  render();
}

/// The bytes of what is in the viewer `v`, read in pieces, as a blob's URL.
async function blobOf(v, mime) {
  const parts = [];
  for (let at = 0; ;) {
    const piece = await call('shelf.bytes', { path: v.path, at });
    if (state.viewer !== v) return '';
    parts.push(fromBase64(piece.data));
    at = v.got = piece.next;
    if (piece.done) break;
    render();
  }
  return URL.createObjectURL(new Blob(parts, { type: mime }));
}

function fromBase64(text) {
  const raw = atob(text);
  const out = new Uint8Array(raw.length);
  for (let i = 0; i < raw.length; i++) out[i] = raw.charCodeAt(i);
  return out;
}

/// A document's words, read out of it once.
async function viewerWords(v) {
  v.mode = 'text';
  if (v.words !== undefined) return;
  v.words = null;
  render();
  const got = await call('shelf.view', { path: v.path, text: true });
  v.words = got.text || '';
  v.wordsNote = got.note || '';
  v.wordsCut = !!got.cut;
}

function closeViewer() {
  if (state.viewer && state.viewer.url) URL.revokeObjectURL(state.viewer.url);
  state.viewer = null;
}

function viewerView() {
  const v = state.viewer;
  const pdf = v.show === 'pdf' && !v.loading && !v.error;
  const modes = pdf ? `<div class="viewer-mode" role="group" aria-label="Show">
      <button data-act="viewer-mode" data-mode="page" aria-pressed="${v.mode === 'page'}" ${v.url ? '' : 'disabled'}>Page</button>
      <button data-act="viewer-mode" data-mode="text" aria-pressed="${v.mode === 'text'}">Text</button></div>` : '';
  return `<div class="shelf-head viewer-head">
      <button class="action small" data-act="back-to-shelf">Back to shelf</button>
      <span class="shelf-title viewer-title" title="${escape(v.path)}">${escape(v.name || '')}</span>
      ${modes}<div class="shelf-acts">${keepButtons(v.path, true)}</div>${savedLine(v.path)}</div>
    ${region('viewer-body', `<div id="viewer-body" class="viewer">${viewerBody(v)}</div>`)}`;
}

function viewerBody(v) {
  const note = (text) => `<div class="viewer-note">${text}</div>`;
  if (v.error) return note(escape(v.error));
  if (v.loading) return note(v.got && v.bytes ? `Reading... ${Math.min(100, Math.round((v.got / v.bytes) * 100))}%` : 'Reading...');
  if (v.show === 'pdf' && v.mode === 'text') {
    return v.words === null ? note('Reading the words...') : wordsView(v.words, v.wordsNote, v.wordsCut);
  }
  const url = escape(v.url || '');
  const name = escape(v.name || '');
  switch (v.show) {
    case 'pdf': return `<iframe class="viewer-frame" src="${url}" title="${name}"></iframe>`;
    case 'image': return `<div class="viewer-picture"><img src="${url}" alt="${name}"></div>`;
    case 'audio': return `<div class="viewer-player"><audio controls src="${url}"></audio></div>`;
    case 'video': return `<div class="viewer-player"><video controls src="${url}"></video></div>`;
    case 'text': return textView(v);
    case 'document': return wordsView(v.text || '', v.note, v.cut);
    case 'archive': return archiveView(v);
    default: return note(apart(escape(SHELF_KINDS[v.kind] || 'File'), bytes(v.bytes), escape(v.note || 'Nothing to show here')));
  }
}

/// What is said under something shown only in part.
const viewerCut = (...said) => {
  const parts = said.filter(Boolean);
  return parts.length ? `<div class="viewer-cut">${apart(...parts.map(escape))}</div>` : '';
};

/// A file's own text: Markdown set, a table as one, code colored.
function textView(v) {
  const text = v.text || '';
  const lang = String(v.language || '').toLowerCase();
  const cut = viewerCut(v.cut ? 'Only the start of it is shown' : '');
  if (text.length > VIEW_COLORED) return `<pre class="viewer-plain">${escape(text)}</pre>${cut}`;
  if (lang === 'md' || lang === 'markdown') return `<div class="viewer-doc md">${markdown(text)}</div>${cut}`;
  if (lang === 'csv' || lang === 'tsv') return tableView(text, lang === 'tsv' ? '\t' : ',') + cut;
  return `<div class="viewer-code">${codeBlock(text, languageOf(v.name || '') || lang)}</div>${cut}`;
}

/// A document's words, as they were read out of it.
function wordsView(text, note, cut) {
  return `<div class="viewer-doc viewer-words">${escape(text || '')}</div>${
    viewerCut(note || '', cut ? 'Only the start of it is shown' : '')}`;
}

/// The rows of a table written as CSV (or TSV, with `sep` a tab), at most
/// `most` of them. Quoted cells keep their commas, quotes and line breaks.
function tableRows(text, sep, most) {
  const rows = [];
  let row = [];
  let cell = '';
  let quoted = false;
  const end = () => { row.push(cell.replace(/\r$/, '')); cell = ''; };
  for (let i = 0; i < text.length && rows.length < most; i++) {
    const c = text[i];
    if (quoted) {
      if (c !== '"') cell += c;
      else if (text[i + 1] === '"') { cell += '"'; i++; } else quoted = false;
    } else if (c === '"' && cell === '') quoted = true;
    else if (c === sep) end();
    else if (c === '\n') { end(); rows.push(row); row = []; } else cell += c;
  }
  if ((cell || row.length) && rows.length < most) { end(); rows.push(row); }
  return rows;
}

function tableView(text, sep) {
  const rows = tableRows(text, sep, VIEW_ROWS + 1);
  const more = rows.length > VIEW_ROWS;
  const [head = [], ...body] = rows.slice(0, VIEW_ROWS);
  const cells = (row, tag) => row.map((c) => `<${tag}>${escape(c)}</${tag}>`).join('');
  return `<div class="viewer-table"><table><thead><tr>${cells(head, 'th')}</tr></thead>
      <tbody>${body.map((row) => `<tr>${cells(row, 'td')}</tr>`).join('')}</tbody></table></div>${
    viewerCut(more ? `The first ${VIEW_ROWS} rows` : '')}`;
}

/// What is in an archive, and how large each is.
function archiveView(v) {
  const entries = v.entries || [];
  const rows = entries.map((e) => `<div class="viewer-entry"><span class="viewer-entry-name">${escape(e.name)}</span>
      <span class="viewer-entry-size">${bytes(e.bytes)}</span></div>`).join('');
  const more = (v.count || 0) - entries.length;
  return `<div class="viewer-list">${rows || '<div class="viewer-note">Empty</div>'}</div>${
    viewerCut(more > 0 ? `and ${count(more, 'more file')}` : '')}`;
}

actions['shelf-view'] = (button) => openViewer(button.dataset.path);
actions['viewer-mode'] = (button) => guard(async () => {
  const v = state.viewer;
  if (!v) return;
  if (button.dataset.mode === 'text') await viewerWords(v);
  else if (v.url) v.mode = 'page';
});

actions['build-enter'] = () => {
  const build = (state.shelf || {}).build || {};
  // A build with a page in it opens on the page; any other on its code.
  if (build.page) {
    state.chatPane = state.shelfTab = 'preview';
    loadPreview(build.page);
    enterPreview();
    render();
  } else {
    setPane(state.shelfTab === 'source' ? 'source' : 'code');
  }
};
actions['back-to-shelf'] = () => setPane('shelf');

/// Keep `path` (or the build): `how` is 'download' or 'save'. One at a
/// time, and the card says so while it goes: an archive of a large build
/// takes a moment.
async function keepIt(path, how) {
  if (state.shelfSaving) return;
  const build = path === BUILD;
  const name = ((state.shelf || {}).build || {}).name || '';
  state.shelfSaving = path;
  render();
  try {
    await guard(async () => {
      if (how === 'save' && window.saveAs) {
        const got = await window.saveAs(build ? { build: true, name } : { path }, window.crucibleToken || '');
        if (got.error) throw new Error(got.error);
        // No dialog to show on this desktop: into the downloads folder.
        if (got.supported !== false) {
          if (got.saved) state.shelfSaved = { path, to: got.saved };
          return;
        }
      }
      const got = build ? await call('shelf.archive', { name }) : await call('shelf.download', { path });
      state.shelfSaved = { path, to: got.saved, downloads: true };
    });
  } finally {
    state.shelfSaving = '';
    render();
  }
}

actions['shelf-download'] = (button) => keepIt(button.dataset.path, 'download');
actions['shelf-save'] = (button) => keepIt(button.dataset.path, 'save');
actions['shelf-show-downloads'] = () => { if (window.showDownloads) window.showDownloads(); };
actions['shelf-delete'] = async (button) => {
  const sure = await confirmIt({ title: `Delete ${button.dataset.name}?`, body: 'The file is deleted.',
                                 yes: 'Delete', no: 'Keep' });
  if (sure) await guard(async () => { await call('shelf.remove', { path: button.dataset.path }); await loadShelf(); });
};
actions['build-remove'] = async () => {
  const sure = await confirmIt({ title: 'Delete the build?', body: 'The code the experts wrote here is deleted.',
                                 yes: 'Delete', no: 'Keep' });
  if (sure) await guard(async () => { await call('shelf.remove', { build: true }); await loadShelf(); });
};

/// The list again when it has changed, and the shelf opened on it when a
/// prompt puts the first thing on it: once a prompt, as before, and never
/// out from under somebody reading the conversation in a narrow window.
let shelfRevSeen = null;
let shelfNamed = '';
onSnapshot.push(() => {
  // The build is named as the chat, which is named after its first prompt.
  const named = state.snapshot.session_name || '';
  if (named !== shelfNamed) {
    shelfNamed = named;
    if (state.shelf && state.shelf.build && !state.shelf.build.project) loadShelf();
  }
  const rev = state.snapshot.shelf_rev || 0;
  if (rev === shelfRevSeen) return;
  const grew = shelfRevSeen !== null && rev > shelfRevSeen;
  shelfRevSeen = rev;
  loadShelf();
  // Grown, whether as an action landed or as the prompt ended: either is
  // the prompt's making. A deletion is made from the list, which is open.
  if (grew) shelfFollows();
});

/// What the shelf shows, made again only when it may have changed. Coloring
/// a long file or parsing a page into its frame for every token of a reply
/// streaming in beside it is what would make the two together slow.
let shelfMade = { pane: '', html: '' };
function shelfBody(pane) {
  if (!shelfKept || shelfMade.pane !== pane) {
    const body = pane === 'code' ? codePane() : pane === 'source' ? sourcePane()
               : pane === 'terminal' ? termPane() : previewView();
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

actions['shelf-toggle'] = () => setPane(shelfOpen() ? 'chat' : 'shelf');

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

/// The first thing a prompt puts on the shelf expands it, on its list: a
/// build is entered by whoever wants to, not by the prompt. Once a prompt,
/// and the caret stays in the box. Collapsed during a prompt, it stays
/// collapsed for the rest of it.
function shelfFollows() {
  const key = promptKey();
  if (shelfOpenedFor === key) return;
  shelfOpenedFor = key;
  if (state.view !== 'chat' || shownPane() !== 'chat' || !shelfFits()) return;
  state.chatPane = 'shelf';
  render();
}

/// What a pane reads as it opens. The tree every time: a turn may have
/// changed it while another pane was up.
function enterPane(pane) {
  if (pane === 'shelf') loadShelf();
  if (pane === 'code') {
    loadTree();
    refreshTabs();
  }
  if (pane === 'source' && !state.source) loadSource();
  if (pane === 'preview' && !state.preview.pages) enterPreview();
  if (pane === 'terminal') enterTerminal();
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
  state.shelfSaved = null;
  if (was) {
    closeViewer();
    state.chatPane = 'chat';
  }
  loadShelf();
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
