// SPDX-License-Identifier: MIT
//
// A build, where the message that asked for it was, and the panes beside the
// conversation.
//
// There is one box. A message that asks for software to be made becomes a
// build (the orchestrator decides, in intent.py), and the build is drawn in
// the conversation where the reply would be: its plan, an agent for each
// piece of it with the expert that has it, and what the chosen one is doing,
// its steps, the files it touched, the file itself. When it is done, what it
// left is the turn's reply, and the conversation carries on from it.
//
// Beside the conversation, once there is a folder to show, the project's
// code, its version control and a preview of what it shows: a person
// watching software get built wants to read it, see it and keep it, not only
// watch a log of it being made.

const CHAT_PANES = [
  ['chat',    'Chat',    'The conversation'],
  ['code',    'Code',    'The project\'s files'],
  ['source',  'Source',  'Version control'],
  ['preview', 'Preview', 'What it shows'],
];

state.chatPane = 'chat';
state.tree = null;
state.source = null;
state.diff = null;
state.log = null;
state.ship = { command: '', tag: '', notes: '', output: '', running: false };
state.teach = null;

/// Builds of this conversation that are not the one the snapshot carries,
/// by id, read from the project's history for the turns they were:
/// { reading }, { journal } or { error }.
state.builds = {};

/// The build running now, or the last one, as the snapshot carries it. A
/// cook started through the API (a build with no plan) is drawn the same
/// way, as one task that is the whole goal.
function currentBuild() {
  return state.snapshot.cook || null;
}

/// The journal of the build `id` for its turn, or null while it is read.
function journalOf(id) {
  const now = currentBuild();
  if (now && now.id === id) return now;
  const kept = state.builds[id];
  return kept && kept.journal ? kept.journal : null;
}

// --- the panes ----------------------------------------------------------------------

/// Whether there is a folder to show beside the conversation: a project, or
/// the chat's own scratch folder once something is in it.
function folderShown() {
  const project = state.snapshot.project || {};
  if (!project.open) return false;
  if (!project.scratchpad) return true;
  return !!(state.tree && state.tree.entries && state.tree.entries.length);
}

/// The pane on screen: the one chosen, while there is a folder to show.
function shownPane() {
  return folderShown() ? state.chatPane : 'chat';
}

/// Above the conversation, its panes. The conversation's tab says when it
/// wants somebody: the build asking, or an edit waiting on a yes.
function paneNav() {
  if (!folderShown()) return '';
  const s = state.snapshot;
  const wanted = !!s.pending_edit || !!(s.cook && s.cook.running && s.cook.state === 'asking');
  const here = shownPane();
  return `<div class="pane-nav">${CHAT_PANES.map(([id, label, tip]) =>
    `<button data-act="chat-pane" data-pane="${id}" aria-current="${here === id}" title="${tip}">${label}${
      id === 'chat' && wanted && here !== 'chat' ? '<span class="pip"></span>' : ''}</button>`).join('')}</div>`;
}

function setPane(pane) {
  state.chatPane = pane;
  render();
  enterPane(pane);
}

actions['chat-pane'] = (button) => setPane(button.dataset.pane);

/// What a pane reads as it opens. The tree every time: a turn or a build
/// may have changed it while another pane was up.
function enterPane(pane) {
  if (pane === 'code') {
    loadTree();
    refreshTabs();
  }
  if (pane === 'source' && !state.source) loadSource();
  if (pane === 'preview' && !state.preview.pages) enterPreview();
}

entering.chat = () => enterPane(shownPane());

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
/// that did anything. A choice is of one build's task (state.open.taskOf),
/// and every other build in the conversation goes on showing its own.
function openTask(build) {
  const tasks = build.tasks || [];
  const mine = !state.open.taskOf || state.open.taskOf === build.id;
  if (mine && state.open.task !== undefined && state.open.task !== null) {
    const chosen = Number(state.open.task);
    if (chosen === -1 || tasks.some((t) => t.index === chosen)) return chosen;
  }
  const working = tasks.find((t) => t.state === 'working');
  if (working) return working.index;
  const touched = tasks.filter((t) => t.state !== 'waiting');
  return touched.length ? touched[touched.length - 1].index : -1;
}

/// The agents, a row each: the architect's own, then one per task. The side
/// menu draws the running build's under the experts, so they are in sight
/// from every view, and each build in the conversation draws its own.
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
            data-act="${act}" data-index="${index}" data-build="${escape(build.id || '')}" data-phase="${phase}"
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

/// The row the side menu marks as open: the task on screen in the
/// conversation, and none anywhere else, where nothing of an agent's shows.
function agentShown(build) {
  return state.view === 'chat' && shownPane() === 'chat' ? openTask(build) : null;
}

/// The agents drawn beside an agent's work: in a build's turn, and in a
/// past build in History.
function agentsList(build, chosen, act) {
  return `<div class="agents"><div class="caption">AGENTS</div>${agentRows(build, chosen, act)}</div>`;
}

/// Open task `index` of `build` in the conversation, and bring its turn into
/// sight: from the side menu that is a click away from wherever it was.
function openAgent(build, index) {
  state.open.task = index;
  state.open.taskOf = build ? build.id || '' : '';
  state.revealBuild = state.open.taskOf;
  state.chatPane = 'chat';
  if (state.view !== 'chat') enter('chat');
  else render();
}

/// Click an agent, in the side menu or a build's turn: its work, there.
actions['agent-open'] = (button) => {
  const id = button.dataset.build || '';
  const build = (id && journalOf(id)) || currentBuild();
  // In the turn already on screen: just that agent, where it is.
  if (state.view === 'chat' && shownPane() === 'chat' && button.closest && button.closest('.build-turn')) {
    state.open.task = Number(button.dataset.index);
    state.open.taskOf = build ? build.id || '' : '';
    render();
    return;
  }
  openAgent(build, Number(button.dataset.index));
};

/// After a draw: the build's turn an agent was opened for, brought into
/// sight once.
afterDraw.push(() => {
  if (!state.revealBuild || state.view !== 'chat') return;
  const card = document.getElementById(`build-${state.revealBuild}`);
  state.revealBuild = '';
  if (card && card.scrollIntoView) card.scrollIntoView({ block: 'nearest' });
});

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
function agentWork(build, chosen, live = true) {
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
  // The latest file only for the build running or just run: it is read for
  // that one, and a past build in History waited on "Reading..." for good.
  return `<div class="work">${head}${taskFiles(build, chosen)}${cookSteps(steps, 0)}${
    live ? latestFile(build, chosen) : ''}</div>`;
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
  if (state.view !== 'chat' || shownPane() !== 'chat') return;
  const build = currentBuild();
  if (!build || !(state.snapshot.turns || []).some((t) => t.build === build.id)) return;
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

/// A build, as its turn draws it under the message that asked for it: how
/// far it is, what it is asking, its agents and the chosen one's work, and
/// once it is over, what it left. `live` is the build the snapshot carries,
/// running or just run, whose agents' files are read as they are now.
function buildCard(build, live) {
  const chosen = openTask(build);
  const running = live && build.running;
  const said = live ? build.state : settled(build);
  const dot = said === 'failed' ? 'missing' : running ? 'active' : 'dormant';
  const tasks = build.tasks || [];
  const done = tasks.filter((t) => t.state === 'done').length;
  // Its two stops are the box's, at hand whichever pane is up, and Stop now
  // is on the turn as a chat turn's Stop is: not here a third time.
  return `<div class="cook-state seat" data-phase="${dot}">
        <span class="dot"></span><strong${running && atWork(build) ? ' class="working"' : ''}>${escape(said)}</strong>
        <span class="status">${apart(tasks.length ? `${done} of ${count(tasks.length, 'task')} done` : 'planning',
          span(build.seconds), count(build.total !== undefined ? build.total : (build.steps || []).length, 'step'))}</span>
      </div>
      ${running && build.state === 'asking' ? `<div class="asking"><div class="caption">IT IS ASKING</div>
          <div>${escape(build.question)}</div>
          <div class="status" style="margin-top:.5rem">answer below</div></div>` : ''}
      ${live && sidebarOpen()
        // The side menu lists this build's agents, beside it: not twice.
        ? `<div class="build-split solo">${agentWork(build, chosen, live)}</div>`
        : `<div class="build-split">${agentsList(build, chosen)}${agentWork(build, chosen, live)}</div>`}
      ${build.outcome && !running ? `<hr class="rule"><div class="md outcome">${markdown(build.outcome)}</div>` : ''}
      ${!running && said !== 'idle' ? (build.files && build.files.length
        ? `<div class="changed"><span class="status">changed</span> ${build.files.map((f) =>
            `<button class="link file-chip" data-act="file-open" data-path="${escape(f)}">${escape(f)}</button>`).join(' ')}</div>`
        : '<div class="changed bad">no files changed</div>') : ''}`;
}

/// A turn that became a build: the message, then the build where its reply
/// would be. A build that cannot be read any more (its journal gone with
/// the history) is what it left, as any reply is.
function buildTurnView(turn, index, busy) {
  const live = currentBuild();
  const running = !!(live && live.running && live.id === turn.build);
  const build = journalOf(turn.build);
  const kept = state.builds[turn.build];
  let body;
  if (build) body = buildCard(build, !!(live && live.id === turn.build));
  else if (kept && kept.error) body = turn.reply ? `<div class="body md">${markdown(turn.reply)}</div>` : '';
  else body = '<div class="status">Reading...</div>';
  // Over, it can be asked again or taken away like any turn; running, the
  // one thing to do with it is stop it.
  const controls = running
    ? `<button class="icon" data-act="build-cancel" title="Stop now" aria-label="Stop now">${ICONS.stop}</button>`
    : busy ? ''
    : `<button class="icon" data-act="turn-retry" data-index="${index}" title="Ask again" aria-label="Ask again">${ICONS.retry}</button>
       <button class="icon" data-act="turn-delete" data-index="${index}" title="Delete" aria-label="Delete">${ICONS.trash}</button>`;
  return `${controls ? `<div class="turn-controls">${controls}</div>` : ''}
      ${attachedChips(turn.attachments)}<div class="said">${escape(turn.prompt)}</div>
      <div class="who"><span class="seat">◆ build</span></div>${body}`;
}

/// The journals of the conversation's builds that are not the snapshot's,
/// read once each from the project's history.
afterDraw.push(() => {
  if (state.view !== 'chat') return;
  const live = currentBuild();
  for (const turn of state.snapshot.turns || []) {
    const id = turn.build;
    if (!id || (live && live.id === id) || state.builds[id]) continue;
    state.builds[id] = { reading: true };
    call('history.cook', { id }).then((journal) => { state.builds[id] = { journal }; })
      .catch((error) => { state.builds[id] = { error: error.message }; })
      .finally(render);
  }
});

// --- the code -----------------------------------------------------------------------------
//
// The editor is code.js: codePane, loadTree and the file and editor actions.

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
/// time, only when the pane is opened or refreshed: not after every step of
/// a build. One look at a time.
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

/// A project change empties what was read of the old one. A chat's scratch
/// folder is looked at at once: its panes show once it has something in it.
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
  state.open.task = null;
  state.open.taskOf = '';
  state.builds = {};
  state.preview = Object.assign(state.preview, { pages: null, page: '', html: '', picked: null, changes: [] });
  if (project.scratchpad) loadTree();
  if (state.view === 'chat') enterPane(shownPane());
});

/// What a turn leaves in a scratch folder is what shows its panes: looked
/// at again whenever a turn ends, until something is there.
let turnsSeen = '';
onSnapshot.push(() => {
  const s = state.snapshot;
  const project = s.project || {};
  const key = `${project.root}|${(s.turns || []).length}|${!!s.busy}`;
  if (key === turnsSeen) return;
  turnsSeen = key;
  if (project.scratchpad && !s.busy && !folderShown()) loadTree();
});

/// A build's step leaves the tree and the status stale, may have rewritten
/// the file on show or the page in the preview, and in a scratch folder may
/// be the first file there is.
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
  // slice(-0) is the whole list: no new steps is none, not all of them.
  const added = Math.max(0, steps - buildSteps);
  const fresh = build && added ? (build.steps || []).slice(-added) : [];
  buildSteps = steps;
  const changed = fresh.flatMap((step) => step.changed || []);
  const touchedPage = changed.some((f) => /\.(html?|css|js|svg|png|jpe?g|gif)$/i.test(f));
  // An agent wrote a file somebody has open: it is read again, or marked.
  codeFilesChanged(changed);
  const pane = state.view === 'chat' ? shownPane() : '';
  if (pane === 'code' || (changed.length && !folderShown())) loadTree();
  if (pane === 'source' && (added || finished)) loadSource(false);
  else if (added || finished) state.source = null;   // read again when its pane opens
  if (pane === 'preview' && (touchedPage || finished)) refreshPreview();
  // A build that has just made or changed a page shows it: what was built
  // is the thing to look at now. Only from the conversation, and not from
  // under somebody typing or waiting on an answer.
  if (finished && build.state === 'done' && pane === 'chat') showWhatWasBuilt();
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
    const typing = !!(state.drafts.chat || '').trim();
    if (!pages.length || state.view !== 'chat' || shownPane() !== 'chat' || state.snapshot.busy || typing) return;
    state.preview.pages = pages;
    state.chatPane = 'preview';
    loadPreview(pages.includes('index.html') ? 'index.html' : pages[0]);
  }).catch(() => {});
}
