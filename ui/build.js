// SPDX-License-Identifier: MIT
//
// Build: a directive, a plan, and an agent for each piece of it.
//
// The view is the whole of the work rather than a transcript of it: the
// agents down one side, each a task with the expert that has it, and what
// the chosen one is doing on the other -- its steps, the files it touched,
// the file itself when one is clicked. Beside that, the project's code, its
// version control and a preview of what it shows, because a person watching
// software get built wants to read it, see it and keep it, not only watch a
// log of it being made.

const BUILD_PANES = [
  ['agents',  'Agents',  'Who is doing what'],
  ['code',    'Code',    'The project\'s files'],
  ['source',  'Source',  'Version control'],
  ['preview', 'Preview', 'What it shows'],
];

const BUILD_OPENERS = [
  'A command-line todo app in Python, with tests',
  'A landing page for a bakery, one HTML file with its CSS',
  'A REST API for notes in Node with Express, with tests and a README',
];

state.buildPane = 'agents';
state.tree = null;
state.source = null;
state.diff = null;
state.log = null;
state.ship = { command: '', tag: '', notes: '', output: '', running: false };
state.teach = null;

/// The build on screen: whatever journal the snapshot carries. A cook
/// started through the API -- a build with no plan -- is drawn the same way,
/// as one task that is the whole goal.
function currentBuild() {
  return state.snapshot.cook || null;
}

/// Which task a step belongs to. The wire leaves the field out for the
/// build's own steps -- the plan, the check, the review -- and those are the
/// architect's row, which is -1.
const taskOf = (step) => (step.task === undefined || step.task === null ? -1 : step.task);

/// Whether a build is at work now: running, and not waiting on an answer.
/// What lights the status line and the working agents in orange.
function atWork(build) {
  return !!(build && build.running && build.state !== 'asking');
}

/// How a task's state is drawn: a seat dot's phase, and a word.
const TASK_PHASE = { waiting: 'unconfigured', working: 'active', done: 'dormant', incomplete: 'missing',
                     failed: 'missing', skipped: 'unconfigured', stopped: 'missing' };

// --- the agents -------------------------------------------------------------------------

/// Which task is open: the one chosen, else the one working, else the last
/// that did anything.
function openTask(build) {
  const tasks = build.tasks || [];
  if (state.open.task !== undefined && state.open.task !== null) {
    const chosen = Number(state.open.task);
    if (chosen === -1 || tasks.some((t) => t.index === chosen)) return chosen;
  }
  const working = tasks.find((t) => t.state === 'working');
  if (working) return working.index;
  const touched = tasks.filter((t) => t.state !== 'waiting');
  return touched.length ? touched[touched.length - 1].index : -1;
}

/// The agents, a row each: the architect's own, then one per task. The side
/// menu draws them under the experts, so they are in sight from every view;
/// the Build pane draws them itself only while the side menu is folded.
/// `chosen` is the row to mark as the one open, or null for none; `act` is
/// what a click does, which in History is to stay on the build it shows.
function agentRows(build, chosen, act = 'agent-open') {
  const tasks = build.tasks || [];
  const architect = (build.steps || []).find((s) => taskOf(s) === -1 && s.kind === 'plan') || {};
  // What an agent at work is doing is said in orange, so a model writing
  // a long file reads as busy rather than stuck.
  const live = atWork(build);
  const row = (index, phase, title, who, extra) => {
    const working = live && phase === 'active';
    return `
    <button class="agent seat${index === chosen ? ' here' : ''}${working ? ' at-work' : ''}"
            data-act="${act}" data-index="${index}" data-phase="${phase}"
            title="${escape(`${title}\n${who}${extra ? `\n${extra}` : ''}`)}">
      <span class="dot"></span>
      <span class="agent-text"><span class="agent-title">${escape(title)}</span>
        <span class="agent-who">${apart(escape(who),
          extra ? (working ? `<span class="working">${escape(extra)}</span>` : escape(extra)) : '')}</span></span>
    </button>`;
  };
  const planning = !tasks.length && build.running;
  return row(-1, planning ? 'active' : 'dormant', 'Architect',
             architect.expert ? expertName(architect.expert) : (planning ? 'planning' : 'the plan and the check'),
             planning ? 'writing the plan' : '')
    + tasks.map((t) => row(t.index, TASK_PHASE[t.state] || 'unconfigured', `${t.index + 1}. ${t.title}`,
                           t.expert ? expertName(t.expert) : (t.needs || 'not yet assigned'), t.state)).join('');
}

/// The row the agents mark as open: the task on screen in the Build pane,
/// and none anywhere else, where nothing of an agent's is showing.
function agentShown(build) {
  return state.view === 'build' && state.buildPane === 'agents' ? openTask(build) : null;
}

/// The agents drawn inside a pane, for the Build view with the side menu
/// folded and for a past build in History.
function agentsList(build, chosen, act) {
  return `<div class="agents"><div class="caption">AGENTS</div>${agentRows(build, chosen, act)}</div>`;
}

/// Click an agent, in the side menu or the pane: its work, in the Build view.
actions['agent-open'] = (button) => {
  state.open.task = Number(button.dataset.index);
  if (state.view === 'build' && state.buildPane === 'agents') {
    render();
    return;
  }
  // The pane is remembered, and entering Build reads it back.
  remember.set('build-pane', 'agents');
  enter('build');
};

/// The files a task changed, as chips that open them.
function taskFiles(build, index) {
  const files = [];
  for (const step of build.steps || []) {
    if (taskOf(step) !== index) continue;
    for (const f of step.changed || []) if (!files.includes(f)) files.push(f);
  }
  if (!files.length) return '';
  return `<div class="changed"><span class="status">files</span> ${files.map((f) =>
    `<button class="link file-chip" data-act="file-open" data-path="${escape(f)}">${escape(f)}</button>`).join(' ')}</div>`;
}

/// The chosen agent's work: what it was asked, what it did, what it said.
function agentWork(build, chosen) {
  const tasks = build.tasks || [];
  const task = tasks.find((t) => t.index === chosen);
  const steps = (build.steps || []).filter((s) => taskOf(s) === chosen);
  let head;
  if (task) {
    head = `<div class="goal-card"><div class="caption">${apart(`TASK ${task.index + 1}`, escape(task.state.toUpperCase()),
        task.expert ? escape(expertName(task.expert)) : '')}</div>
      <div class="goal">${escape(task.title)}</div>
      <div class="hint" style="margin-top:.4rem;white-space:pre-wrap">${escape(task.detail)}</div>
      ${task.needs ? `<div class="hint">needs: ${escape(task.needs)}</div>` : ''}
      ${task.after && task.after.length ? `<div class="hint">after task${task.after.length > 1 ? 's' : ''} ${
        task.after.map((a) => a + 1).join(', ')}</div>` : ''}
      ${task.outcome ? `<div class="md outcome" style="margin-top:.5rem">${markdown(task.outcome)}</div>` : ''}</div>`;
  } else {
    const plan = build.plan || {};
    head = `<div class="goal-card"><div class="caption">THE PLAN</div>
      <div class="md">${plan.summary ? markdown(plan.summary) : '<span class="status">Not written yet.</span>'}</div>
      ${plan.run ? `<div class="hint">run: <code>${escape(plan.run)}</code></div>` : ''}
      ${plan.check ? `<div class="hint">check: <code>${escape(plan.check)}</code></div>` : ''}
      ${plan.ship ? `<div class="hint">ship: <code>${escape(plan.ship)}</code></div>` : ''}</div>`;
  }
  return `<div class="work">${head}${taskFiles(build, chosen)}${cookSteps(steps, 0)}${latestFile(build, chosen)}</div>`;
}

/// The file the chosen agent last wrote, as it is now: clicking an agent
/// goes to its code, not only to a list of what it did. Read when the
/// choice changes, and drawn once it has arrived.
function latestFile(build, chosen) {
  const latest = lastChanged(build, chosen);
  if (!latest) return '';
  const file = state.open.taskFile;
  if (!file || file.path !== latest) return `<div class="status" style="margin-top:1rem">Reading ${escape(latest)}...</div>`;
  if (file.error) return `<div class="bad" style="margin-top:1rem">${escape(file.error)}</div>`;
  if (file.image) return `<div class="caption" style="margin-top:1.2rem">${escape(latest)}</div>${
    file.thumb ? `<img class="code-picture" src="${file.thumb}" alt="${escape(latest)}">` : ''}`;
  return `<div class="row code-head-row" style="margin-top:1.2rem"><strong class="file-name">${escape(latest)}</strong>
      <span class="status">${file.bytes ? bytes(file.bytes) : ''}</span><span class="spacer"></span>
      <button class="action small" data-act="file-open" data-path="${escape(latest)}">Open in Code</button></div>
    ${codeBlock(file.content || '', languageOf(latest) || file.language)}`;
}

/// The path the agent of task `index` changed most recently, or ''.
function lastChanged(build, index) {
  let latest = '';
  for (const step of build.steps || []) {
    if (taskOf(step) !== index) continue;
    const changed = step.changed || [];
    if (changed.length) latest = changed[changed.length - 1];
  }
  return latest;
}

/// Keep the chosen agent's latest file read: after each draw, if the file
/// shown is not the one it last wrote, ask for it once.
let readingTaskFile = '';
afterDraw.push(() => {
  if (state.view !== 'build' || state.buildPane !== 'agents') return;
  const build = currentBuild();
  if (!build) return;
  const latest = lastChanged(build, openTask(build));
  const have = state.open.taskFile;
  if (!latest || (have && have.path === latest && !have.stale) || readingTaskFile === latest) return;
  readingTaskFile = latest;
  call('project.read', { path: latest }).then(async (got) => {
    const file = Object.assign({ path: latest }, got);
    if (got.image) {
      try {
        const picture = await call('attach.image', { path: got.full });
        file.thumb = `data:${picture.mime};base64,${picture.data}`;
      } catch (e) { /* shown without its picture */ }
    }
    state.open.taskFile = file;
  }).catch((error) => { state.open.taskFile = { path: latest, error: error.message }; })
    .finally(() => { readingTaskFile = ''; render(); });
});

function buildBody(build) {
  const chosen = openTask(build);
  const running = build.running;
  const dot = build.state === 'failed' ? 'missing' : running ? 'active' : 'dormant';
  const tasks = build.tasks || [];
  const done = tasks.filter((t) => t.state === 'done').length;
  return `<div class="goal-card">
        <div class="caption">DIRECTIVE</div>
        <div class="goal">${escape(build.goal)}</div>
        ${attachedChips(build.attachments)}
        ${running ? `<button class="icon goal-stop" data-act="build-cancel" title="Stop now" aria-label="Stop now">${ICONS.stop}</button>` : ''}
      </div>
      <div class="cook-state seat" data-phase="${dot}">
        <span class="dot"></span><strong${atWork(build) ? ' class="working"' : ''}>${escape(build.state)}</strong>
        <span class="status">${apart(tasks.length ? `${done} of ${count(tasks.length, 'task')} done` : 'planning',
          span(build.seconds), count(build.total !== undefined ? build.total : (build.steps || []).length, 'step'))}</span>
      </div>
      ${build.state === 'asking' ? `<div class="asking"><div class="caption">IT IS ASKING</div>
          <div>${escape(build.question)}</div>
          <div class="status" style="margin-top:.5rem">answer below</div></div>` : ''}
      ${sidebarOpen() ? `<div class="build-split solo">${agentWork(build, chosen)}</div>`
        : `<div class="build-split">${agentsList(build, chosen)}${agentWork(build, chosen)}</div>`}
      ${build.outcome && !running ? `<hr class="rule"><div class="md outcome">${markdown(build.outcome)}</div>` : ''}
      ${!running && build.state !== 'idle' ? (build.files && build.files.length
        ? `<div class="changed"><span class="status">changed</span> ${build.files.map((f) =>
            `<button class="link file-chip" data-act="file-open" data-path="${escape(f)}">${escape(f)}</button>`).join(' ')}</div>`
        : '<div class="changed bad">no files changed</div>') : ''}`;
}

// --- the code -----------------------------------------------------------------------------

function codePane() {
  const project = state.snapshot.project || {};
  if (!project.open) return '<div class="empty"><div class="empty-label">No project open.</div></div>';
  const tree = state.tree;
  const open = state.open.file;
  const entries = !tree ? '<div class="status">Reading...</div>'
    : !tree.entries.length ? '<div class="status">Nothing in it yet.</div>'
    : tree.entries.map((e) => `<button class="tree-row${open && open.path === e.path ? ' here' : ''}${e.dir ? ' dir' : ''}"
          data-act="${e.dir ? 'tree-none' : 'file-open'}" data-path="${escape(e.path)}" style="padding-left:${0.6 + e.depth * 0.9}rem"
          title="${escape(e.path)}">${escape(e.name)}${e.dir ? '/' : ''}</button>`).join('')
      + (tree.cut ? '<div class="status" style="padding:.4rem .6rem">more files not listed</div>' : '');
  let file = '<div class="status" style="padding:1rem">Click a file to read it.</div>';
  if (open) {
    if (open.loading) {
      file = '<div class="status" style="padding:1rem">Reading...</div>';
    } else if (open.error) {
      file = `<div class="bad" style="padding:1rem">${escape(open.error)}</div>`;
    } else if (open.image) {
      file = open.thumb ? `<img class="code-picture" src="${open.thumb}" alt="${escape(open.path)}">`
                        : '<div class="status" style="padding:1rem">Loading the picture...</div>';
    } else if (open.editing) {
      // The text is typed into a textarea whose own letters are clear, over
      // the same text colored: highlighted as it is edited, and still a
      // textarea -- undo, selection, the keyboard -- underneath.
      file = `<div class="code-editor-wrap">
          <pre class="code-editor-under" aria-hidden="true"><code>${highlight(open.draft, fileLanguage(open))}\n</code></pre>
          <textarea class="code-editor" data-key="editor-key" data-input="editor-change" data-scroll="editor-scroll"
                spellcheck="false" data-draft aria-label="Editing ${escape(open.path)}">${escape(open.draft)}</textarea></div>`;
    } else {
      file = codeBlock(open.content || '', fileLanguage(open))
           + (open.cut ? '<div class="status">cut: the start of a long file</div>' : '');
    }
  }
  const head = open ? `<div class="row code-head-row"><strong class="file-name">${escape(open.path)}</strong>
      <span class="status">${open.bytes ? bytes(open.bytes) : ''}</span><span class="spacer"></span>
      ${open.image || open.loading || open.error ? '' : open.editing
        ? `<button class="action small" data-act="editor-save" ${state.snapshot.busy ? 'disabled' : ''}>Save</button>
           <button class="action small" data-act="editor-cancel">Cancel</button>`
        : `<button class="action small" data-act="editor-edit">Edit</button>`}
      <button class="action small" data-act="file-close">Close</button></div>` : '';
  return `<div class="code-split">
      <div class="tree"><div class="row" style="padding:0 .6rem .4rem"><div class="caption" style="margin:0;flex:1">FILES</div>
        <button class="link" data-act="tree-refresh">Refresh</button></div>${entries}</div>
      <div class="code-view">${head}${file}</div>
    </div>`;
}

async function loadTree() {
  if (!(state.snapshot.project || {}).open) return;
  try { state.tree = await call('project.tree'); } catch (error) { state.error = error.message; }
  render();
}

actions['tree-refresh'] = () => { state.tree = null; render(); loadTree(); };
actions['tree-none'] = () => {};
actions['file-open'] = (button) => guard(async () => {
  const path = button.dataset.path;
  state.open.file = { path, loading: true };
  if (state.view === 'build') state.buildPane = 'code';
  render();
  try {
    const got = await call('project.read', { path });
    state.open.file = Object.assign({ path }, got, { loading: false });
    if (got.image) {
      const picture = await call('attach.image', { path: got.full });
      state.open.file.thumb = `data:${picture.mime};base64,${picture.data}`;
    }
  } catch (error) {
    state.open.file = { path, error: error.message };
  }
});
actions['file-close'] = () => { state.open.file = null; render(); };
actions['editor-edit'] = () => { const f = state.open.file; f.editing = true; f.draft = f.content || ''; render(); };
actions['editor-cancel'] = () => { const f = state.open.file; f.editing = false; render(); };
/// The language an open file is read as: by its name first, which knows a
/// Dockerfile, and then by what the reader said.
const fileLanguage = (file) => languageOf(file.path) || file.language || '';

actions['editor-change'] = (box) => {
  const f = state.open.file;
  f.draft = box.value;
  // The colors under the text, redrawn here rather than by a render: one per
  // keystroke would draw the whole window again for one letter.
  const under = box.previousElementSibling;
  if (under && under.firstElementChild) {
    under.firstElementChild.innerHTML = highlight(box.value, fileLanguage(f)) + '\n';
  }
};
actions['editor-scroll'] = (box) => {
  const under = box.previousElementSibling;
  if (under) { under.scrollTop = box.scrollTop; under.scrollLeft = box.scrollLeft; }
};
actions['editor-key'] = (box, event) => {
  if (event.key === 'Tab') { event.preventDefault(); const at = box.selectionStart;
    box.value = box.value.slice(0, at) + '    ' + box.value.slice(box.selectionEnd);
    box.selectionStart = box.selectionEnd = at + 4; actions['editor-change'](box); }
  if ((event.ctrlKey || event.metaKey) && (event.key === 's' || event.key === 'S')) {
    event.preventDefault(); actions['editor-save']();
  }
};
actions['editor-save'] = () => guard(async () => {
  const f = state.open.file;
  await call('project.write', { path: f.path, content: f.draft });
  f.content = f.draft;
  f.editing = false;
  state.source = null;
  loadSource();
});

// --- version control --------------------------------------------------------------------

function sourcePane() {
  const project = state.snapshot.project || {};
  if (!project.open) return '<div class="empty"><div class="empty-label">No project open.</div></div>';
  const s = state.source;
  if (!s) return '<div class="status" style="padding:1rem">Reading...</div>';
  if (!s.git) return `<div class="empty"><div class="empty-label">git is not installed</div>
      <div class="status">Crucible runs the git on this machine, and there is none.</div></div>`;
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
  const plan = (currentBuild() || {}).plan || {};
  return `<div class="source">
      <div class="row source-head"><strong>${escape(s.branch || '(no branch)')}</strong>
        <span class="status">${apart(s.remote ? escape(s.remote) : 'no remote', s.ahead ? `${s.ahead} ahead` : '',
          s.behind ? `${s.behind} behind` : '')}</span>
        <span class="spacer"></span>
        <button class="action small" data-act="source-refresh">Refresh</button>
        ${s.remote ? `<button class="action small" data-act="git-pull" ${busy ? 'disabled' : ''}>Pull</button>
                      <button class="action small" data-act="git-push" ${busy ? 'disabled' : ''}>Push</button>`
                   : s.gh ? `<button class="action small" data-act="git-publish" ${busy ? 'disabled' : ''}>Publish to GitHub</button>`
                   : '<span class="hint" title="Install gh, GitHub\'s command line, to publish from here">no gh</span>'}</div>
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
      <div class="hint">Package it, then tag a release${s.gh ? ' on GitHub' : ' (gh is needed for a GitHub release)'}.</div>
      <form class="row" data-submit="ship-package" style="margin-top:.5rem">
        <input id="ship-command" data-draft value="${escape(ship.command || plan.ship || '')}"
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
        <span class="hint">A page of the project can be made a program that opens like any other.</span>
        <button class="action small" data-act="build-pane" data-pane="preview">Make it an app</button></div>
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

async function loadSource() {
  if (!(state.snapshot.project || {}).open) return;
  try {
    state.source = await call('git.status');
    if (state.source.repo) state.log = (await call('git.log')).commits; else state.log = null;
    // CI only where there is some to ask about: a GitHub remote, and gh.
    state.ci = state.source.gh && /github\.com/.test(state.source.remote || '') ? await call('git.ci') : null;
  } catch (error) { state.error = error.message; }
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
    body: 'A private repository under your account, through gh, with everything committed so far.',
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

// --- the view -------------------------------------------------------------------------------

views.build = () => {
  const s = state.snapshot;
  const build = currentBuild();
  const nav = `<div class="build-nav">${BUILD_PANES.map(([id, label, tip]) =>
    `<button data-act="build-pane" data-pane="${id}" aria-current="${state.buildPane === id}" title="${tip}">${label}</button>`).join('')}</div>`;
  let body;
  if (state.buildPane === 'code') body = codePane();
  else if (state.buildPane === 'source') body = sourcePane();
  else if (state.buildPane === 'preview') body = previewView();
  else body = build ? buildBody(build)
    : readiness(`<div class="empty"><div class="empty-label quiet">Give it a directive</div>
        <div class="status">It plans, gives each task to an expert, builds, checks, and writes it up.</div>
        <div class="chips">${BUILD_OPENERS.map((text) => `<button class="chip" data-act="opener">${escape(text)}</button>`).join('')}</div></div>`);
  const edit = s.pending_edit && state.buildPane === 'agents' ? pendingEdit(s.pending_edit) : '';
  // The switcher sits above the scroller, not inside it: a long build's
  // journal scrolled it out of reach, and the panes it leads to are the ones
  // wanted most while a build is long.
  // Only the journal keeps to its bottom as it grows; the other panes are
  // read from the top, and a tree or a preview toolbar scrolled out of sight
  // is a pane with nothing to click.
  const journal = state.buildPane === 'agents';
  return sideView() + `<div class="pane with-nav">${nav}
      <div class="scroller"><div id="${journal ? 'transcript' : 'pane-body'}" class="build-transcript">${body}${edit}</div>
        <button class="jump" id="jump" data-act="jump" hidden>${ICONS.down} Jump to latest</button></div>
      ${composerView({ build: true, hint: 'what should it build?', send: 'Build' })}
    </div>` + recentsView();
};

actions['build-pane'] = (button) => {
  state.buildPane = button.dataset.pane;
  remember.set('build-pane', state.buildPane);
  render();
  enterPane(state.buildPane);
};

function enterPane(pane) {
  if (pane === 'code' && !state.tree) loadTree();
  if (pane === 'source' && !state.source) loadSource();
  if (pane === 'preview' && !state.preview.pages) enterPreview();
}

entering.build = () => {
  state.buildPane = remember.get('build-pane', 'agents');
  enterPane(state.buildPane);
};

/// A project change empties what was read of the old one.
let buildProject = '';
onSnapshot.push(() => {
  const root = (state.snapshot.project || {}).root || '';
  if (root === buildProject) return;
  buildProject = root;
  state.tree = null;
  state.source = null;
  state.diff = null;
  state.log = null;
  state.open.file = null;
  state.open.task = null;
  state.preview = Object.assign(state.preview, { pages: null, page: '', html: '', picked: null, changes: [] });
  if (state.view === 'build') enterPane(state.buildPane);
});

/// A build that just finished leaves the tree and the status stale -- and
/// another step by an agent may have rewritten the file on show, or the page
/// in the preview.
let buildWas = '';
let buildRunning = false;
let buildSteps = 0;
onSnapshot.push(() => {
  const build = currentBuild();
  const steps = build ? (build.total || (build.steps || []).length) : 0;
  const now = build ? `${build.id}|${build.running}|${(build.files || []).length}|${steps}` : '';
  if (now === buildWas) return;
  buildWas = now;
  const finished = buildRunning && build && !build.running;
  buildRunning = !!(build && build.running);
  if (state.open.taskFile) state.open.taskFile.stale = true;
  // What changed since the last look: a page, a stylesheet or a script.
  const fresh = build ? (build.steps || []).slice(-Math.max(0, steps - buildSteps)) : [];
  buildSteps = steps;
  const touchedPage = fresh.some((step) => (step.changed || []).some((f) => /\.(html?|css|js|svg|png|jpe?g|gif)$/i.test(f)));
  if (state.view === 'build') {
    if (state.buildPane === 'code') loadTree();
    if (state.buildPane === 'source') loadSource();
    if (state.buildPane === 'preview' && (touchedPage || finished)) refreshPreview();
    // A build that has just made or changed a page shows it, from the
    // agents' pane: what was built is the thing to look at now. Not from a
    // pane the person chose to be on.
    if (finished && build.state === 'done' && state.buildPane === 'agents') showWhatWasBuilt();
  } else {
    state.tree = null;
    state.source = null;
  }
});

/// The page again, unless the person is in the middle of tuning it: then it
/// waits, and the strip under the bar says it has changed.
function refreshPreview() {
  const p = state.preview;
  if (p.url) return;
  if (p.changes.length || p.picked || p.making) { p.stale = true; return; }
  if (p.page) loadPreview(p.page); else enterPreview();
}

/// After a build: its preview, when the project has a page to show.
function showWhatWasBuilt() {
  call('preview.candidates').then((got) => {
    const pages = got.pages || [];
    if (!pages.length || state.view !== 'build' || state.buildPane !== 'agents') return;
    state.preview.pages = pages;
    state.buildPane = 'preview';
    remember.set('build-pane', 'preview');
    loadPreview(pages.includes('index.html') ? 'index.html' : pages[0]);
  }).catch(() => {});
}
