// SPDX-License-Identifier: MIT
//
// Code: the project's files, open in tabs and changed in place.
//
// Agents do most of the typing in a build, but somebody reading what an agent
// wrote wants to fix the line without leaving for another program. So this is
// a small editor rather than a viewer: files in tabs, line numbers, find and
// replace, a search across the project, new, rename and delete, and a box to
// run a command in. What is typed is colored as it is typed, by the same
// lexer the chat uses, drawn under a textarea that keeps the keyboard, the
// selection and undo.
//
// Saving is allowed while a build runs. What keeps it from losing an agent's
// work, and an agent from losing the person's, is the fingerprint each file
// was read with: when the disk no longer matches it, a save asks first, and a
// tab with nothing unsaved in it is read again. See project.write.

const CODE_ICONS = {
  file: '<svg viewBox="0 0 16 16" width="14" height="14" aria-hidden="true" fill="none" stroke="currentColor" stroke-width="1.3" stroke-linejoin="round"><path d="M4 1.8h5.2L12.5 5v9.2H4z"/><path d="M9 1.8V5h3.5"/><path d="M8.2 7.5v4.4M6 9.7h4.4" stroke-linecap="round"/></svg>',
  folder: '<svg viewBox="0 0 16 16" width="14" height="14" aria-hidden="true" fill="none" stroke="currentColor" stroke-width="1.3" stroke-linejoin="round"><path d="M1.8 4.2h4.4l1.4 1.5h6.6v7.6H1.8z"/><path d="M8 7.6v4M6 9.6h4" stroke-linecap="round"/></svg>',
  refresh: '<svg viewBox="0 0 16 16" width="14" height="14" aria-hidden="true" fill="none" stroke="currentColor" stroke-width="1.3" stroke-linecap="round" stroke-linejoin="round"><path d="M13.2 6.5A5.3 5.3 0 1 0 13 10"/><path d="M13.5 2.8v3.9H9.6"/></svg>',
  close: '<svg viewBox="0 0 16 16" width="10" height="10" aria-hidden="true" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round"><path d="M4 4l8 8M12 4l-8 8"/></svg>',
  open: '<svg viewBox="0 0 16 16" width="10" height="10" aria-hidden="true" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"><path d="M4.5 6l3.5 3.5L11.5 6"/></svg>',
  shut: '<svg viewBox="0 0 16 16" width="10" height="10" aria-hidden="true" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"><path d="M6 4.5L9.5 8 6 11.5"/></svg>',
};

state.code = {
  tabs: [],          // every file open, in the order opened
  closed: null,      // folders folded in the tree, by path: see foldedFolders
  side: 'files',     // 'files' or 'search'
  find: null,        // the find bar: { mode: 'find'|'replace'|'line', query, replace, matchCase, at }
  search: { query: '', results: null, cut: false, matchCase: false, busy: false },
  run: { open: false, command: '', output: '', running: false, history: [], back: -1 },
  serial: 0,         // a number per tab, so each has a textarea of its own
};

/// Folders folded in the tree. Read when first wanted rather than when this
/// script loads: the remembered settings arrive with start(), after it.
function foldedFolders() {
  if (!state.code.closed) state.code.closed = remember.get('code-closed', {}) || {};
  return state.code.closed;
}

/// The file on screen. Kept as state.open.file so the agents' views, which
/// open a file with file-open, need not know about tabs.
const activeTab = () => state.open.file || null;
const tabOf = (path) => state.code.tabs.find((t) => t.path === path);
const unsaved = (tab) => !!(tab && tab.editable && tab.draft !== undefined && tab.draft !== tab.content);

/// The indent a file already uses: a tab when its lines start with tabs more
/// than with spaces, else the smallest run of spaces that starts a line.
function indentOf(text) {
  let tabs = 0;
  let spaces = 0;
  let unit = 0;
  for (const line of String(text || '').split('\n', 2000)) {
    if (line.startsWith('\t')) { tabs++; continue; }
    const lead = line.match(/^ +(?=\S)/);
    if (!lead) continue;
    spaces++;
    const n = lead[0].length;
    if (n >= 2 && (unit === 0 || n < unit)) unit = n;
  }
  if (tabs > spaces) return '\t';
  return ' '.repeat(unit === 2 ? 2 : 4);
}

/// What starts a line comment in a language, or '' when there is none.
function commentOf(lang) {
  const l = String(lang || '').toLowerCase();
  if (/^(py|python|sh|bash|zsh|shell|rb|ruby|yml|yaml|toml|r|pl|perl|cmake|makefile|dockerfile|gitignore|ini|conf|editorconfig|nim|jl|julia|ex|exs|elixir)$/.test(l)) return '#';
  if (/^(sql|lua|hs|haskell|elm)$/.test(l)) return '--';
  if (/^(tex|latex|m|matlab)$/.test(l)) return '%';
  if (/^(lisp|clj|clojure|scm|el)$/.test(l)) return ';';
  if (/^(vb|bas)$/.test(l)) return "'";
  if (/^(md|markdown|txt|text|json|csv|html?|xml|svg|css)$/.test(l)) return '';
  return '//';
}

// --- reading and keeping ------------------------------------------------------------------

/// Read a file into a tab: its text with line ends as the editor holds them
/// (\n), and how they were on disk, so a save gives them back unchanged.
function fill(tab, got) {
  // Read again: a new textarea, since the one on screen keeps the text it was
  // given and would go on showing the old file over the new colors.
  if (tab.content !== undefined) tab.serial = ++state.code.serial;
  Object.assign(tab, got, { loading: false, error: '', stale: false, conflict: false, gone: false });
  const text = String(got.content || '');
  tab.crlf = /\r\n/.test(text);
  tab.content = tab.crlf ? text.replace(/\r\n/g, '\n') : text;
  tab.draft = tab.content;
  tab.indent = indentOf(tab.content);
}

async function readInto(tab) {
  try {
    const got = await call('project.read', { path: tab.path });
    fill(tab, got);
    if (got.image) {
      const picture = await call('attach.image', { path: got.full });
      tab.thumb = `data:${picture.mime};base64,${picture.data}`;
    }
  } catch (error) {
    tab.loading = false;
    tab.error = error.message;
  }
}

/// Open `path` in a tab and show it, at `line` when one is given.
async function openFile(path, line) {
  let tab = tabOf(path);
  if (!tab) {
    tab = { path, loading: true, serial: ++state.code.serial };
    state.code.tabs.push(tab);
  }
  if (tab.error && !unsaved(tab)) { tab.error = ''; tab.loading = true; }
  state.open.file = tab;
  // A file opened from the conversation (a chip a turn or a build left) is
  // opened in the Code pane beside it.
  if (state.view === 'chat' && state.chatPane !== 'code') {
    state.chatPane = 'code';
    if (!state.tree) loadTree();
  }
  if (line) tab.goto = line;
  render();
  if (tab.loading) {
    await readInto(tab);
    render();
  }
}

/// Look again at the open tabs: one with nothing unsaved whose file has
/// changed is read again; one with changes is marked, and says so.
async function refreshTabs() {
  for (const tab of state.code.tabs.slice()) {
    if (tab.loading || tab.image) continue;
    try {
      const got = await call('project.read', { path: tab.path });
      if (got.hash === tab.hash && !tab.error && !tab.gone) continue;
      if (unsaved(tab)) { tab.conflict = !tab.gone || got.hash !== tab.hash; tab.diskHash = got.hash; tab.gone = false; }
      else fill(tab, got);
    } catch (error) {
      // Deleted or moved from under the tab. With nothing unsaved in it the
      // tab goes with the file; with something, it stays and says so.
      if (unsaved(tab)) tab.gone = true;
      else closeTab(tab.path);
    }
  }
  render();
}

/// Files a build step just wrote: the open ones among them are looked at.
function codeFilesChanged(paths) {
  if (!paths.some((p) => tabOf(p))) return;
  refreshTabs();
}

async function saveTab(tab, force) {
  if (!tab || !tab.editable || tab.saving) return;
  const draft = tab.draft;
  tab.saving = true;
  render();
  try {
    const content = tab.crlf ? draft.replace(/\n/g, '\r\n') : draft;
    const got = await call('project.write', { path: tab.path, content, expected: tab.hash || '', force: !!force });
    tab.content = draft;
    tab.hash = got.hash;
    tab.conflict = false;
    tab.gone = false;
    state.source = null;
  } catch (error) {
    tab.saving = false;
    if (/changed on disk/.test(error.message)) {
      const sure = await confirmIt({ title: `${fileName(tab.path)} changed`,
        body: 'It changed since you opened it, maybe by an agent. Save yours over it?',
        yes: 'Save mine', no: 'Cancel' });
      if (sure) return saveTab(tab, true);
      tab.conflict = true;
    } else {
      state.error = error.message;
    }
  }
  tab.saving = false;
  render();
}

async function closeTab(path) {
  const tab = tabOf(path);
  if (!tab) return;
  if (unsaved(tab)) {
    const discard = await confirmIt({ title: `Unsaved changes to ${fileName(path)}`, body: 'Close it anyway?',
                                      yes: 'Discard', no: 'Keep' });
    if (!discard) return;
  }
  const at = state.code.tabs.indexOf(tab);
  state.code.tabs.splice(at, 1);
  if (activeTab() === tab) {
    state.open.file = state.code.tabs[Math.min(at, state.code.tabs.length - 1)] || null;
  }
  render();
}

/// Whether any tab has work in it that is not on disk: asked before the
/// project changes under the editor.
function codeUnsaved() {
  return state.code.tabs.filter(unsaved).map((t) => t.path);
}

// --- the tree -------------------------------------------------------------------------------

/// The rows the tree shows: everything but what is inside a folded folder.
function treeRows(entries) {
  const closed = foldedFolders();
  const folded = Object.keys(closed).filter((p) => closed[p]);
  return entries.filter((e) => !folded.some((dir) => e.path.startsWith(dir + '/')));
}

function treeView() {
  const tree = state.tree;
  const open = activeTab();
  if (!tree) return '<div class="status" style="padding:.4rem .6rem">Reading...</div>';
  if (!tree.entries.length) return '<div class="status" style="padding:.4rem .6rem">Nothing in it yet.</div>';
  const rows = treeRows(tree.entries).map((e) => {
    const folded = !!foldedFolders()[e.path];
    const tab = tabOf(e.path);
    return `<button class="tree-row${open && open.path === e.path ? ' here' : ''}${e.dir ? ' dir' : ''}"
        data-act="${e.dir ? 'tree-fold' : 'file-open'}" data-path="${escape(e.path)}"
        style="padding-left:${0.4 + e.depth * 0.85}rem" title="${escape(e.path)}">${e.dir
          ? `<span class="tree-mark">${folded ? CODE_ICONS.shut : CODE_ICONS.open}</span>` : '<span class="tree-mark"></span>'}${
        escape(e.name)}${unsaved(tab) ? '<span class="dirty-dot" title="Unsaved"></span>' : ''}</button>`;
  }).join('');
  return rows + (tree.cut ? '<div class="status" style="padding:.4rem .6rem">more files not listed</div>' : '');
}

function searchView() {
  const s = state.code.search;
  const results = s.results;
  let list = '';
  if (s.busy) list = '<div class="status">Searching...</div>';
  else if (results && !results.length) list = '<div class="status">Nothing found.</div>';
  else if (results) {
    let last = '';
    list = results.map((r) => {
      const head = r.path !== last ? `<div class="search-file" title="${escape(r.path)}">${escape(r.path)}</div>` : '';
      last = r.path;
      return `${head}<button class="search-hit" data-act="search-open" data-path="${escape(r.path)}" data-line="${r.line}">
          <span class="search-line">${r.line}</span>${escape(r.text.trim())}</button>`;
    }).join('') + (s.cut ? '<div class="status">more not shown</div>' : '');
  }
  return `<form class="row search-form" data-submit="search-run">
      <input id="search-query" data-draft data-input="search-typed" value="${escape(s.query)}" placeholder="Search files"
             aria-label="Search files" autocomplete="off" spellcheck="false">
      <button type="button" class="icon toggle small${s.matchCase ? ' on' : ''}" aria-pressed="${s.matchCase}"
              data-act="search-case" title="Match case">Aa</button></form>
    <div class="search-results">${list}</div>`;
}

// --- the editor -----------------------------------------------------------------------------

function tabsView() {
  const open = activeTab();
  const tabs = state.code.tabs.slice();
  if (open && !tabs.includes(open)) tabs.push(open);   // set by somebody else
  if (!tabs.length) return '';
  return `<div class="ide-tabs" role="tablist">${tabs.map((t) => `
      <div class="ide-tab${t === open ? ' here' : ''}" role="tab" aria-selected="${t === open}" title="${escape(t.path)}">
        <button class="ide-tab-name" data-act="tab-show" data-path="${escape(t.path)}">${escape(fileName(t.path))}</button>
        ${unsaved(t) ? '<span class="dirty-dot" title="Unsaved"></span>' : ''}
        <button class="ide-tab-close" data-act="tab-close" data-path="${escape(t.path)}" aria-label="Close ${escape(fileName(t.path))}">${CODE_ICONS.close}</button>
      </div>`).join('')}</div>`;
}

function findView(tab) {
  const f = state.code.find;
  if (!f || !tab || !tab.editable) return '';
  if (f.mode === 'line') {
    return `<form class="ide-find row" data-submit="find-line">
        <input id="find-line" data-draft data-key="find-key" placeholder="Line" aria-label="Go to line"
               autocomplete="off" style="max-width:8rem">
        <span class="status">of ${(tab.draft || '').split('\n').length}</span>
        <span class="spacer"></span>
        <button type="button" class="icon small" data-act="find-close" aria-label="Close">${CODE_ICONS.close}</button></form>`;
  }
  const matches = findMatches(tab.draft || '', f.query, f.matchCase);
  const where = !f.query ? '' : matches.length ? `${Math.min(f.at + 1, matches.length)} of ${matches.length}` : 'none';
  return `<div class="ide-find">
      <form class="row" data-submit="find-next">
        <input id="find-query" data-draft data-input="find-typed" data-key="find-key" value="${escape(f.query)}"
               placeholder="Find" aria-label="Find" autocomplete="off" spellcheck="false">
        <span class="status find-count">${where}</span>
        <button type="button" class="icon toggle small${f.matchCase ? ' on' : ''}" aria-pressed="${f.matchCase}"
                data-act="find-case" title="Match case">Aa</button>
        <button type="button" class="action small" data-act="find-prev" title="Previous (Shift+Enter)">Up</button>
        <button class="action small" title="Next (Enter)">Down</button>
        <button type="button" class="link small" data-act="find-mode">${f.mode === 'replace' ? 'Find only' : 'Replace'}</button>
        <span class="spacer"></span>
        <button type="button" class="icon small" data-act="find-close" aria-label="Close">${CODE_ICONS.close}</button></form>
      ${f.mode === 'replace' ? `<form class="row" data-submit="find-replace" style="margin-top:.35rem">
        <input id="find-with" data-draft data-input="find-with" data-key="find-key" value="${escape(f.replace)}"
               placeholder="Replace with" aria-label="Replace with" autocomplete="off" spellcheck="false">
        <button class="action small">Replace</button>
        <button type="button" class="action small" data-act="find-all">All</button></form>` : ''}
    </div>`;
}

function editorView(tab) {
  if (tab.loading) return '<div class="status" style="padding:1rem">Reading...</div>';
  if (tab.error) return `<div class="bad" style="padding:1rem">${escape(tab.error)}</div>`;
  if (tab.image) {
    return tab.thumb ? `<img class="code-picture" src="${tab.thumb}" alt="${escape(tab.path)}">`
                     : '<div class="status" style="padding:1rem">Loading the picture...</div>';
  }
  const lang = fileLanguage(tab);
  if (!tab.editable) {
    // A PDF's words, a binary's bytes, the start of a huge file: something
    // to read, never to save back.
    return codeBlock(tab.content || '', lang)
      + `<div class="status">${tab.cut ? 'Too large to edit here. The start of it.' : 'Read only.'}</div>`;
  }
  const lines = (tab.draft.match(/\n/g) || []).length + 1;
  // Very long files are edited without the colors under them: coloring the
  // whole file on every key is what would make typing in it lag.
  const plain = tab.draft.length > 200000;
  // The numbers are inside the box, over the textarea's left padding, so
  // the textarea alone sets how tall the editor is: beside it, a column of
  // every line's number made the box as tall as the file.
  return `<div class="ide-editor${plain ? ' plain' : ''}" style="--gutter:${String(lines).length}ch">
      <div class="code-editor-wrap">
        ${plain ? '' : `<pre class="code-editor-under" aria-hidden="true"><code>${highlight(tab.draft, lang)}\n</code></pre>`}
        <textarea class="code-editor" id="editor-${tab.serial || 0}" wrap="off" spellcheck="false" data-draft
                  data-key="editor-key" data-input="editor-change" data-scroll="editor-scroll" data-act="editor-cursor"
                  aria-label="Editing ${escape(tab.path)}">\n${escape(tab.draft)}</textarea>
        <pre class="ide-gutter" aria-hidden="true" data-lines="${lines}">${gutterNumbers(lines)}</pre></div></div>`;
}

const gutterNumbers = (n) => Array.from({ length: n }, (_, i) => i + 1).join('\n');

function statusView(tab) {
  if (!tab || !tab.editable) return '';
  const lang = fileLanguage(tab);
  const cursor = tab.cursor || { line: 1, col: 1 };
  return `<div class="ide-status">
      <span id="ide-cursor">Ln ${cursor.line}, Col ${cursor.col}</span>
      <span>${escape(LANG_NAMES[String(lang).toLowerCase()] || lang || 'Text')}</span>
      <span>${tab.crlf ? 'CRLF' : 'LF'}</span>
      <span>${tab.indent === '\t' ? 'Tabs' : `${(tab.indent || '    ').length} spaces`}</span>
      <span class="spacer"></span>
      <button class="link small" data-act="run-toggle" aria-pressed="${state.code.run.open}">Terminal</button></div>`;
}

function terminalView() {
  const r = state.code.run;
  if (!r.open) return '';
  return `<div class="ide-run">
      <form class="row" data-submit="run-command">
        <span class="status">$</span>
        <input id="run-command" data-draft data-key="run-key" value="${escape(r.command)}" placeholder="a command, in the project"
               aria-label="Command" autocomplete="off" spellcheck="false">
        <button class="action small" ${r.running ? 'disabled' : ''}>${r.running ? 'Running...' : 'Run'}</button></form>
      ${r.output ? codeBlock(r.output, 'shell') : ''}</div>`;
}

/// The Code pane: the files on one side, the open ones on the other.
function codePane() {
  const project = state.snapshot.project || {};
  if (!project.open) return '<div class="empty"><div class="empty-label">No project open.</div></div>';
  const tab = activeTab();
  const side = state.code.side;
  const head = `<div class="ide-side-head">
      <button class="ide-side-tab" data-act="code-side" data-side="files" aria-current="${side === 'files'}">Files</button>
      <button class="ide-side-tab" data-act="code-side" data-side="search" aria-current="${side === 'search'}" title="Ctrl+Shift+F">Search</button>
      <span class="spacer"></span>
      ${side === 'files' ? `<button class="icon small" data-act="code-new" title="New file">${CODE_ICONS.file}</button>
        <button class="icon small" data-act="code-new" data-dir="1" title="New folder">${CODE_ICONS.folder}</button>
        <button class="icon small" data-act="tree-refresh" title="Refresh">${CODE_ICONS.refresh}</button>` : ''}</div>`;
  let main;
  if (!tab) {
    main = `<div class="empty"><div class="empty-label quiet">Open a file</div>
        <div class="status">Ctrl+P to find one by name</div></div>`;
  } else {
    const dirty = unsaved(tab);
    const bar = `<div class="row ide-bar"><strong class="file-name" title="${escape(tab.path)}">${escape(tab.path)}</strong>
        <span class="status">${tab.bytes ? bytes(tab.bytes) : ''}</span><span class="spacer"></span>
        ${tab.editable ? `<button class="action small" data-act="editor-save" ${dirty && !tab.saving ? '' : 'disabled'}
            title="Ctrl+S">${tab.saving ? 'Saving...' : 'Save'}</button>
          ${dirty ? '<button class="action small" data-act="editor-revert">Revert</button>' : ''}` : ''}
        <button class="action small" data-act="code-rename" data-path="${escape(tab.path)}">Rename</button>
        <button class="action small" data-act="code-delete" data-path="${escape(tab.path)}">Delete</button></div>`;
    const conflict = tab.conflict ? `<div class="ide-banner row"><span>Changed on disk since you opened it.</span>
        <span class="spacer"></span>
        <button class="action small" data-act="conflict-reload">Use the disk's</button>
        <button class="action small" data-act="conflict-keep">Keep mine</button></div>`
      : tab.gone ? `<div class="ide-banner row"><span>Gone from disk. Save puts it back.</span>
        <span class="spacer"></span>
        <button class="action small" data-act="tab-close" data-path="${escape(tab.path)}">Close</button></div>` : '';
    // The banner and the find bar each have a slot that is always there:
    // the page is patched child by child in order, and one appearing above
    // the editor would otherwise rebuild it, undo history and all.
    main = tabsView() + bar + `<div class="ide-slot" data-key="banner">${conflict}</div>`
      + `<div class="ide-slot" data-key="find">${findView(tab)}</div>`
      + editorView(tab) + statusView(tab) + terminalView();
  }
  return `<div class="ide">
      <div class="ide-side">${head}<div class="ide-side-body">${side === 'search' ? searchView() : treeView()}</div></div>
      <div class="ide-main">${main}</div>
    </div>`;
}

/// The language an open file is read as: by its name first, which knows a
/// Dockerfile, and then by what the reader said.
const fileLanguage = (file) => languageOf(file.path) || file.language || '';

async function loadTree() {
  if (!(state.snapshot.project || {}).open) return;
  try { state.tree = await call('project.tree'); } catch (error) { state.error = error.message; }
  render();
}

// --- keeping the screen in step with the textarea -------------------------------------------

/// The textarea on screen, when it is the open tab's.
const editorBox = () => document.querySelector('textarea.code-editor');

/// Recolor and renumber what is under the textarea after it changed, without
/// a render: one per keystroke would draw the whole window again for a letter.
function repaint(box) {
  const tab = activeTab();
  if (!tab) return;
  const editor = box.closest('.ide-editor');
  const under = box.previousElementSibling;
  if (under && under.firstElementChild) {
    under.firstElementChild.innerHTML = highlight(box.value, fileLanguage(tab)) + '\n';
  }
  const gutter = editor && editor.querySelector('.ide-gutter');
  const lines = (box.value.match(/\n/g) || []).length + 1;
  if (gutter && gutter.dataset.lines !== String(lines)) {
    gutter.textContent = gutterNumbers(lines);
    gutter.dataset.lines = String(lines);
    editor.style.setProperty('--gutter', `${String(lines).length}ch`);
  }
  follow(box);
}

/// Keep the colors and the numbers where the text is.
function follow(box) {
  const under = box.previousElementSibling;
  if (under && under.tagName === 'PRE') { under.scrollTop = box.scrollTop; under.scrollLeft = box.scrollLeft; }
  const editor = box.closest('.ide-editor');
  const gutter = editor && editor.querySelector('.ide-gutter');
  if (gutter) gutter.scrollTop = box.scrollTop;
}

/// Where the caret is, for the status line. Written straight into it.
function noteCursor(box) {
  const tab = activeTab();
  if (!tab || !box) return;
  const before = box.value.slice(0, box.selectionStart);
  const line = (before.match(/\n/g) || []).length + 1;
  const col = before.length - before.lastIndexOf('\n');
  tab.cursor = { line, col };
  const shown = document.getElementById('ide-cursor');
  if (shown) shown.textContent = `Ln ${line}, Col ${col}`;
}

/// Scroll so line `line` (from 1) is in view, a few lines down from the top.
function revealLine(box, line) {
  const height = parseFloat(getComputedStyle(box).lineHeight) || 20;
  box.scrollTop = Math.max(0, (line - 4) * height);
  follow(box);
}

/// Replace `from`..`to` with `text` the way typing would, so Ctrl+Z takes it
/// back; and put the selection at `selectFrom`..`selectTo` afterwards.
function typeInto(box, from, to, text, selectFrom, selectTo) {
  box.focus();
  box.setSelectionRange(from, to);
  let typed = false;
  try { typed = document.execCommand('insertText', false, text); } catch (e) { typed = false; }
  if (!typed) box.setRangeText(text, from, to, 'end');
  const end = from + text.length;
  box.setSelectionRange(selectFrom === undefined ? end : selectFrom, selectTo === undefined ? end : selectTo);
  actions['editor-change'](box);
}

/// The start of the line `at` is on, and the end of the line `to` is on.
function lineSpan(text, at, to) {
  const start = text.lastIndexOf('\n', at - 1) + 1;
  let end = text.indexOf('\n', to > at && text[to - 1] === '\n' ? to - 1 : to);
  if (end < 0) end = text.length;
  return [start, end];
}

afterDraw.push(() => {
  const box = editorBox();
  const tab = activeTab();
  if (!box || !tab) return;
  follow(box);
  // A tab opened at a line, from a search or an agent's error: put the caret
  // there once, then forget it.
  if (tab.goto) {
    const line = tab.goto;
    tab.goto = 0;
    const lines = box.value.split('\n');
    const offset = lines.slice(0, Math.max(0, line - 1)).join('\n').length + (line > 1 ? 1 : 0);
    box.focus();
    box.setSelectionRange(offset, offset);
    revealLine(box, line);
    noteCursor(box);
  }
});

// --- the actions ------------------------------------------------------------------------------

actions['tree-refresh'] = () => { state.tree = null; render(); loadTree(); refreshTabs(); };
actions['tree-fold'] = (button) => {
  const path = button.dataset.path;
  const closed = foldedFolders();
  closed[path] = !closed[path];
  remember.set('code-closed', closed);
  render();
};
actions['file-open'] = (button) => guard(() => openFile(button.dataset.path, Number(button.dataset.line) || 0));
actions['tab-show'] = (button) => { const t = tabOf(button.dataset.path); if (t) { state.open.file = t; render(); } };
actions['tab-close'] = (button) => closeTab(button.dataset.path);
// The old pane's Close, kept for whoever still asks for it by name.
actions['file-close'] = () => { const t = activeTab(); if (t) closeTab(t.path); };
actions['code-side'] = (button) => {
  state.code.side = button.dataset.side;
  render();
  if (state.code.side === 'search') setTimeout(() => { const q = document.getElementById('search-query'); if (q) q.focus(); }, 0);
};

actions['editor-change'] = (box) => {
  const tab = activeTab();
  if (!tab) return;
  const was = unsaved(tab);
  tab.draft = box.value;
  repaint(box);
  noteCursor(box);
  // The unsaved marks and the Save button change only when the answer to
  // "unsaved?" does: then, and only then, the window is drawn again.
  if (unsaved(tab) !== was) render();
};
actions['editor-scroll'] = (box) => follow(box);
actions['editor-cursor'] = (box) => noteCursor(box);
actions['editor-save'] = () => saveTab(activeTab());
actions['editor-revert'] = () => {
  const tab = activeTab();
  if (!tab) return;
  tab.draft = tab.content;
  tab.serial = ++state.code.serial;   // a fresh textarea, so the text is the saved text
  render();
};
// The old pane's Edit and Cancel: a file is always editable now.
actions['editor-edit'] = () => render();
actions['editor-cancel'] = () => actions['editor-revert']();

actions['conflict-reload'] = () => guard(async () => {
  const tab = activeTab();
  if (!tab) return;
  tab.loading = true;
  render();
  await readInto(tab);
  tab.serial = ++state.code.serial;
});
actions['conflict-keep'] = () => { const tab = activeTab(); if (tab) tab.conflict = false; render(); };

/// The editor's own keys. Everything else is the textarea's.
actions['editor-key'] = (box, event) => {
  const tab = activeTab();
  if (!tab) return;
  const ctrl = event.ctrlKey || event.metaKey;
  const key = event.key;
  const text = box.value;
  const start = box.selectionStart;
  const end = box.selectionEnd;
  const indent = tab.indent || '    ';
  setTimeout(() => noteCursor(box), 0);

  if (ctrl && !event.altKey && (key === 's' || key === 'S')) { event.preventDefault(); saveTab(tab); return; }
  if (ctrl && !event.shiftKey && (key === 'f' || key === 'F')) { event.preventDefault(); openFind('find', box); return; }
  if (ctrl && (key === 'h' || key === 'H')) { event.preventDefault(); openFind('replace', box); return; }
  if (ctrl && (key === 'g' || key === 'G')) { event.preventDefault(); openFind('line', box); return; }
  if (key === 'Escape' && state.code.find) { event.preventDefault(); actions['find-close'](); return; }

  if (ctrl && key === '/') {
    event.preventDefault();
    const mark = commentOf(fileLanguage(tab));
    if (!mark) return;
    const [from, to] = lineSpan(text, start, end);
    const lines = text.slice(from, to).split('\n');
    const commented = lines.filter((l) => l.trim()).every((l) => l.trimStart().startsWith(mark));
    const changed = lines.map((l) => {
      if (!l.trim()) return l;
      if (commented) return l.replace(new RegExp(`^(\\s*)${mark.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')} ?`), '$1');
      const lead = l.match(/^\s*/)[0];
      return lead + mark + ' ' + l.slice(lead.length);
    }).join('\n');
    typeInto(box, from, to, changed, from, from + changed.length);
    return;
  }

  if (event.altKey && (key === 'ArrowUp' || key === 'ArrowDown')) {
    // Move the lines the selection is on up or down one.
    event.preventDefault();
    const [from, to] = lineSpan(text, start, end);
    const block = text.slice(from, to);
    if (key === 'ArrowUp') {
      if (from === 0) return;
      const above = text.lastIndexOf('\n', from - 2) + 1;
      const prior = text.slice(above, from - 1);
      typeInto(box, above, to, block + '\n' + prior, above + (start - from), above + (end - from));
    } else {
      if (to >= text.length) return;
      let below = text.indexOf('\n', to + 1);
      if (below < 0) below = text.length;
      const next = text.slice(to + 1, below);
      const shift = next.length + 1;
      typeInto(box, from, below, next + '\n' + block, start + shift, end + shift);
    }
    return;
  }

  if (key === 'Tab' && !ctrl && !event.altKey) {
    event.preventDefault();
    const many = text.slice(start, end).includes('\n');
    if (!many && !event.shiftKey) { typeInto(box, start, end, indent); return; }
    // A block: every line it touches, in or out by one level.
    const [from, to] = lineSpan(text, start, end);
    const lines = text.slice(from, to).split('\n');
    const changed = lines.map((l) => {
      if (!event.shiftKey) return l.trim() ? indent + l : l;
      if (l.startsWith('\t')) return l.slice(1);
      const lead = l.match(/^ */)[0].length;
      return l.slice(Math.min(lead, indent === '\t' ? 4 : indent.length));
    }).join('\n');
    typeInto(box, from, to, changed, from, from + changed.length);
    return;
  }

  if (key === 'Enter' && !ctrl && !event.altKey && !event.shiftKey && start === end) {
    // Keep the line's indent, and go one further after an opener.
    event.preventDefault();
    const lineStart = text.lastIndexOf('\n', start - 1) + 1;
    const line = text.slice(lineStart, start);
    const lead = line.match(/^\s*/)[0];
    const opens = /[{[(]\s*$/.test(line) || (/:\s*$/.test(line) && /^(py|python|yml|yaml|nim)$/i.test(fileLanguage(tab)));
    const closer = { '{': '}', '[': ']', '(': ')' }[line.trimEnd().slice(-1)];
    if (opens && closer && text[start] === closer) {
      // Between a pair: the closer goes to a line of its own.
      const inner = '\n' + lead + indent;
      typeInto(box, start, end, inner + '\n' + lead, start + inner.length, start + inner.length);
      return;
    }
    typeInto(box, start, end, '\n' + lead + (opens ? indent : ''));
    return;
  }

  if ((key === '}' || key === ']' || key === ')') && !ctrl && start === end) {
    // A closer typed on a line of nothing but indent steps back a level.
    const lineStart = text.lastIndexOf('\n', start - 1) + 1;
    const line = text.slice(lineStart, start);
    if (line.length && !line.trim() && line.length >= (indent === '\t' ? 1 : indent.length)) {
      event.preventDefault();
      const back = line.endsWith('\t') ? 1 : Math.min(line.length, indent.length);
      typeInto(box, start - back, start, key);
    }
  }
};

// --- find, replace, go to line ----------------------------------------------------------------

function findMatches(text, query, matchCase) {
  if (!query) return [];
  const hay = matchCase ? text : text.toLowerCase();
  const needle = matchCase ? query : query.toLowerCase();
  const found = [];
  for (let at = hay.indexOf(needle); at >= 0 && found.length < 10000; at = hay.indexOf(needle, at + needle.length)) {
    found.push(at);
  }
  return found;
}

function openFind(mode, box) {
  const picked = box ? box.value.slice(box.selectionStart, box.selectionEnd) : '';
  const was = state.code.find;
  state.code.find = { mode, query: picked && !picked.includes('\n') ? picked : (was ? was.query : ''),
                      replace: was ? was.replace : '', matchCase: was ? was.matchCase : false, at: 0, focus: true };
  render();
}

/// The find bar's field takes the caret as the bar opens: dialogs do that
/// through data-focus, which is theirs alone.
afterDraw.push(() => {
  const f = state.code.find;
  if (!f || !f.focus) return;
  const input = document.getElementById(f.mode === 'line' ? 'find-line' : 'find-query');
  if (!input) return;
  f.focus = false;
  input.focus();
  input.select();
});

/// Select match number `at` in the textarea and scroll to it.
function showMatch(step) {
  const f = state.code.find;
  const tab = activeTab();
  const box = editorBox();
  if (!f || !tab || !box || !f.query) return;
  const matches = findMatches(box.value, f.query, f.matchCase);
  if (!matches.length) { render(); return; }
  // From the caret, the way every editor does: the next one after it.
  if (step !== 0) {
    const caret = box.selectionEnd;
    if (step > 0) {
      const next = matches.findIndex((m) => m >= caret);
      f.at = next < 0 ? 0 : next;
    } else {
      const from = box.selectionStart;
      let prev = -1;
      matches.forEach((m, i) => { if (m < from) prev = i; });
      f.at = prev < 0 ? matches.length - 1 : prev;
    }
  }
  const at = matches[Math.min(f.at, matches.length - 1)];
  box.setSelectionRange(at, at + f.query.length);
  revealLine(box, (box.value.slice(0, at).match(/\n/g) || []).length + 1);
  noteCursor(box);
  render();
}

actions['find-typed'] = (input) => { state.code.find.query = input.value; state.code.find.at = 0; showMatch(0); };
actions['find-with'] = (input) => { state.code.find.replace = input.value; };
actions['find-case'] = () => { state.code.find.matchCase = !state.code.find.matchCase; showMatch(0); };
actions['find-mode'] = () => { const f = state.code.find; f.mode = f.mode === 'replace' ? 'find' : 'replace'; render(); };
actions['find-next'] = () => showMatch(1);
actions['find-prev'] = () => showMatch(-1);
actions['find-close'] = () => { state.code.find = null; render(); const box = editorBox(); if (box) box.focus(); };
actions['find-key'] = (input, event) => {
  if (event.key === 'Escape') { event.preventDefault(); actions['find-close'](); }
  if (event.key === 'Enter' && event.shiftKey && state.code.find.mode !== 'line') { event.preventDefault(); showMatch(-1); }
};
actions['find-replace'] = () => {
  const f = state.code.find;
  const box = editorBox();
  if (!box || !f.query) return;
  const picked = box.value.slice(box.selectionStart, box.selectionEnd);
  const same = f.matchCase ? picked === f.query : picked.toLowerCase() === f.query.toLowerCase();
  if (same) typeInto(box, box.selectionStart, box.selectionEnd, f.replace);
  showMatch(1);
  const field = document.getElementById('find-with');
  if (field) field.focus();
};
actions['find-all'] = () => {
  const f = state.code.find;
  const box = editorBox();
  if (!box || !f.query) return;
  const matches = findMatches(box.value, f.query, f.matchCase);
  if (!matches.length) return;
  // One edit for all of them, so one Ctrl+Z takes them all back.
  let out = '';
  let last = 0;
  for (const m of matches) { out += box.value.slice(last, m) + f.replace; last = m + f.query.length; }
  out += box.value.slice(last);
  typeInto(box, 0, box.value.length, out, 0, 0);
  state.error = '';
  state.snapshot.status = `replaced ${count(matches.length, 'match', 'matches')}`;
  render();
};
actions['find-line'] = (form) => {
  const line = Number(form.querySelector('input').value);
  const box = editorBox();
  const tab = activeTab();
  if (!box || !tab || !line) return;
  tab.goto = Math.max(1, Math.min(line, box.value.split('\n').length));
  state.code.find = null;
  render();
};

// --- search across the files -------------------------------------------------------------------

actions['search-typed'] = (input) => { state.code.search.query = input.value; };
actions['search-case'] = () => { state.code.search.matchCase = !state.code.search.matchCase; actions['search-run'](); };
actions['search-run'] = () => guard(async () => {
  const s = state.code.search;
  if (!s.query.trim()) { s.results = null; render(); return; }
  s.busy = true;
  render();
  try {
    const got = await call('project.search', { query: s.query, case: s.matchCase });
    s.results = got.results;
    s.cut = got.cut;
  } finally {
    s.busy = false;
  }
});
actions['search-open'] = (button) => guard(() => openFile(button.dataset.path, Number(button.dataset.line) || 1));

// --- new, rename, delete -----------------------------------------------------------------------

modals['code-name'] = (m) => `<div class="modal small">
    <div class="head"><strong>${escape(m.title)}</strong></div>
    <form class="body-pad" data-submit="code-name-ok">
      <input id="code-name" data-draft data-focus value="${escape(m.value)}" autocomplete="off" spellcheck="false"
             aria-label="${escape(m.title)}">
      ${m.error ? `<div class="bad" style="margin-top:.6rem">${escape(m.error)}</div>` : ''}
    </form>
    <div class="feet">
      <button class="action" data-act="code-name-ok">${escape(m.verb)}</button>
      <button class="action" data-act="modal-close">Cancel</button></div></div>`;

/// The folder a new file goes in: the open file's, or the top.
function hereDir() {
  const tab = activeTab();
  if (!tab) return '';
  const cut = tab.path.lastIndexOf('/');
  return cut > 0 ? tab.path.slice(0, cut + 1) : '';
}

actions['code-new'] = (button) => {
  const dir = !!button.dataset.dir;
  openModal({ kind: 'code-name', title: dir ? 'New folder' : 'New file', verb: 'Create', value: hereDir(),
    done: async (path) => {
      await call('project.create', { path, dir });
      await loadTree();
      if (!dir) await openFile(path);
    } });
};
actions['code-rename'] = (button) => {
  const from = button.dataset.path;
  openModal({ kind: 'code-name', title: `Rename ${fileName(from)}`, verb: 'Rename', value: from,
    done: async (to) => {
      if (to === from) return;
      await call('project.rename', { path: from, to });
      const tab = tabOf(from);
      if (tab) tab.path = to;
      await loadTree();
      render();
    } });
};
actions['code-name-ok'] = () => guard(async () => {
  const m = state.modal;
  const box = document.getElementById('code-name');
  const value = (box ? box.value : m.value).trim();
  if (!value) { m.error = 'It needs a name.'; render(); return; }
  try {
    await m.done(value);
    closeModal();
  } catch (error) {
    m.error = error.message;
    m.value = value;
    render();
  }
});
actions['code-delete'] = async (button) => {
  const path = button.dataset.path;
  const sure = await confirmIt({ title: `Delete ${fileName(path)}?`, body: path, yes: 'Delete', no: 'Keep' });
  if (!sure) return;
  await guard(async () => {
    await call('project.delete', { path });
    const tab = tabOf(path);
    if (tab) { tab.draft = tab.content; await closeTab(path); }
    await loadTree();
  });
};

// --- quick open --------------------------------------------------------------------------------

/// Whether every letter of `query` is in `path`, in order: how "bsjs" finds
/// "build.js". Scored by how early and how close together they are.
function fuzzy(path, query) {
  const p = path.toLowerCase();
  const q = query.toLowerCase().replace(/\s+/g, '');
  let at = -1;
  let score = 0;
  for (const c of q) {
    const next = p.indexOf(c, at + 1);
    if (next < 0) return -1;
    score += next - at;
    at = next;
  }
  // A match in the file's own name beats one spread over its folders.
  return score - (p.slice(p.lastIndexOf('/') + 1).includes(q) ? 1000 : 0);
}

function quickMatches(query) {
  const files = ((state.tree && state.tree.entries) || []).filter((e) => !e.dir);
  if (!query) return files.slice(0, 50);
  return files.map((e) => [fuzzy(e.path, query), e]).filter(([s]) => s >= 0)
    .sort((a, b) => a[0] - b[0]).slice(0, 50).map(([, e]) => e);
}

modals['quick-open'] = (m) => {
  const found = quickMatches(m.query);
  return `<div class="modal quick-open">
    <div class="body-pad">
      <input id="quick-query" data-draft data-focus data-input="quick-typed" data-key="quick-key" value="${escape(m.query)}"
             placeholder="Open a file by name" aria-label="Open a file by name" autocomplete="off" spellcheck="false">
      <div class="quick-list">${found.length ? found.map((e, i) => `
        <button class="tree-row${i === m.at ? ' here' : ''}" data-act="quick-pick" data-path="${escape(e.path)}">
          ${escape(fileName(e.path))} <span class="status">${escape(e.path)}</span></button>`).join('')
        : '<div class="status">No file by that name.</div>'}</div>
    </div></div>`;
};
function openQuick() {
  if (!state.tree) loadTree();
  openModal({ kind: 'quick-open', query: '', at: 0 });
}
actions['quick-typed'] = (input) => { state.modal.query = input.value; state.modal.at = 0; render(); };
actions['quick-key'] = (input, event) => {
  const m = state.modal;
  const found = quickMatches(m.query);
  if (event.key === 'ArrowDown') { event.preventDefault(); m.at = Math.min(found.length - 1, m.at + 1); render(); }
  if (event.key === 'ArrowUp') { event.preventDefault(); m.at = Math.max(0, m.at - 1); render(); }
  if (event.key === 'Enter') {
    event.preventDefault();
    const pick = found[m.at];
    if (pick) { closeModal(); guard(() => openFile(pick.path)); }
  }
};
actions['quick-pick'] = (button) => { closeModal(); guard(() => openFile(button.dataset.path)); };

// --- a command, in the project -------------------------------------------------------------------

actions['run-toggle'] = () => {
  state.code.run.open = !state.code.run.open;
  render();
  if (state.code.run.open) setTimeout(() => { const box = document.getElementById('run-command'); if (box) box.focus(); }, 0);
};
actions['run-command'] = (form) => guard(async () => {
  const r = state.code.run;
  const command = form.querySelector('input').value.trim();
  if (!command || r.running) return;
  r.command = '';
  r.history.push(command);
  r.back = -1;
  r.running = true;
  r.output = `$ ${command}\n`;
  form.querySelector('input').value = '';
  render();
  try {
    const got = await call('project.run', { command, timeout: 600 });
    r.output = `$ ${command}\n${got.output || ''}${got.ok ? '' : `\n(${got.summary})`}`;
  } finally {
    r.running = false;
    // What it may have made or changed.
    loadTree();
    refreshTabs();
  }
});
actions['run-key'] = (input, event) => {
  const r = state.code.run;
  if (!r.history.length || (event.key !== 'ArrowUp' && event.key !== 'ArrowDown')) return;
  event.preventDefault();
  r.back = event.key === 'ArrowUp' ? Math.min(r.history.length - 1, r.back + 1) : Math.max(-1, r.back - 1);
  input.value = r.back < 0 ? '' : r.history[r.history.length - 1 - r.back];
};

// --- keys that work anywhere in the pane ----------------------------------------------------------

/// Ctrl+P and Ctrl+Shift+F while the Code pane is up, wherever focus is; and
/// Ctrl+S, which the textarea handles itself when it has the focus.
function codeShortcut(event) {
  if (event.defaultPrevented || state.view !== 'chat' || shownPane() !== 'code' || state.modal) return;
  const ctrl = event.ctrlKey || event.metaKey;
  if (!ctrl || event.altKey) return;
  if (event.key === 'p' || event.key === 'P') { event.preventDefault(); openQuick(); }
  if (event.shiftKey && (event.key === 'f' || event.key === 'F')) {
    event.preventDefault();
    actions['code-side']({ dataset: { side: 'search' } });
  }
  if (!event.shiftKey && (event.key === 's' || event.key === 'S')) { event.preventDefault(); saveTab(activeTab()); }
}

