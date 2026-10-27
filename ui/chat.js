// SPDX-License-Identifier: MIT
//
// The conversation: what was asked, who answered and how, the builds that
// messages became, and the one box the next message goes in.

// --- one turn -------------------------------------------------------------------

/// Who answered, and how the delegator got there. Above the reply, because it
/// is the first thing to know about what follows -- and it is the only place
/// the routing is visible. Without it a delegating program looks like a chat
/// box.
function turnWho(turn) {
  const route = turn.route;
  // A route that named nobody -- no expert had a model -- is Crucible itself
  // saying so, not a nameless seat.
  const who = route && route.expert ? expertName(route.expert) : 'crucible';
  const bits = [];
  if (route && route.confidence > 0) bits.push(`${Math.round(route.confidence * 100)}%`);
  // How it was routed, only when it was not the usual way: the delegator
  // choosing is what every turn says, so it says nothing. Pinned, a fallback
  // and keywords are the exceptions, and worth a word.
  if (route && route.source && route.source !== 'router model') bits.push(route.source);
  // Only when it actually swapped. A turn answered by the model already
  // resident paid nothing, and a "0.0s" there is noise.
  if (turn.load_ms > 0) bits.push(`swapped in ${(turn.load_ms / 1000).toFixed(1)}s`);
  const tip = 'How sure the delegator was, and what decided it.'
            + (route && route.detail ? '\n' + route.detail : '');
  return `<div class="who" title="${escape(tip)}"><span class="seat">◆ ${escape(who)}</span>${
    bits.map((b) => `<span>${escape(b)}</span>`).join('')}</div>`;
}

/// What the turn cost, under it.
function turnMeta(turn) {
  const bits = [];
  if (turn.tokens_per_second > 0) bits.push(`${turn.tokens_per_second.toFixed(1)} tok/s`);
  if (turn.prompt_tokens > 0) bits.push(`${compact(turn.prompt_tokens)} in`);
  if (turn.output_tokens > 0) bits.push(`${compact(turn.output_tokens)} out`);
  if (turn.canceled) bits.push('stopped');
  return bits.length ? `<div class="meta">${apart(...bits.map(escape))}</div>` : '';
}

/// What an expert did on the way to answering: the files it read, the
/// commands it ran, each where it happened. In line rather than folded away,
/// because a write to your project is not a footnote to the sentence that
/// follows it.
function turnActions(turn) {
  return (turn.actions || []).map((a) => `<div class="act">${
    escape(a.summary)}${pictureChip(a.picture)}${actionBody(a)}</div>`).join('');
}

/// A picture an action or a step produced -- a screenshot, a picture it
/// looked at -- as a chip that opens it. The bytes are fetched when it is
/// opened; a journal carries the path.
function pictureChip(path) {
  if (!path) return '';
  return ` <button class="link picture-chip" data-act="picture-open" data-path="${escape(path)}"
      title="${escape(path)}">picture</button>`;
}

modals.picture = (m) => `<div class="modal wide">
    <div class="head"><strong>${escape(fileName(m.path))}</strong><div class="status">${escape(m.path)}</div></div>
    <div class="body-pad" style="text-align:center">${m.error ? `<div class="bad">${escape(m.error)}</div>`
      : m.src ? `<img class="picture-full" src="${m.src}" alt="${escape(fileName(m.path))}">` : '<div class="status">Loading...</div>'}</div>
    <div class="feet"><button class="action" data-act="modal-close">Close</button></div></div>`;

actions['picture-open'] = (button) => {
  const modal = { kind: 'picture', path: button.dataset.path, src: '', error: '' };
  openModal(modal);
  call('attach.image', { path: modal.path })
    .then((got) => { modal.src = `data:${got.mime};base64,${got.data}`; })
    .catch((error) => { modal.error = error.message; })
    .finally(render);
};

/// What an action left to look at. A write names the file it wrote, and is
/// drawn as the block that was allowed; anything else is its output.
function actionBody(action) {
  // A command that printed nothing has nothing to show: a box with no lines
  // in it reads as the interface broken, not as a quiet command.
  if (!action.body || !action.body.trim()) return '';
  if (action.language && /^@@ line \d+ @@/.test(action.body)) {
    return writtenBlock(action.body, action.language);
  }
  return codeBlock(action.body, action.language || '');
}

/// What can be done to a turn, which depends on what is happening.
///
/// While something is running the only honest offer is to stop it, and only
/// on the turn that is actually running: retrying or deleting a turn
/// mid-flight would renumber the one the engine is writing into. The row is
/// revealed by hovering the whole turn rather than the buttons, because a
/// control that only appears once the pointer is on top of it can never be
/// found.
function turnControls(turn, index, busy) {
  const buttons = turn.streaming
    ? `<button class="icon" data-act="stop" title="Stop" aria-label="Stop">${ICONS.stop}</button>`
    : busy ? ''
    : `<button class="icon" data-act="turn-retry" data-index="${index}" title="Ask again"
               aria-label="Ask again">${ICONS.retry}</button>
       <button class="icon" data-act="turn-delete" data-index="${index}" title="Delete"
               aria-label="Delete">${ICONS.trash}</button>`;
  return buttons ? `<div class="turn-controls">${buttons}</div>` : '';
}

/// Which turns have their thinking open, by conversation and place. Each
/// starts closed, and opening one opens that one.
const thoughtsOpen = new Set();
const thoughtKey = (index) => `${state.snapshot.session || ''}|${index}`;

/// One exchange, as markup.
function turnView(turn, index, busy) {
  let reply;
  if (turn.failed) {
    reply = `<div class="body failed">${escape(turn.reply || 'it could not answer')}</div>`;
  } else if (!turn.reply && turn.streaming) {
    reply = '<div class="body thinking">...</div>';
  } else {
    reply = `<div class="body md">${markdown(turn.reply)}</div>`;
  }
  // Reasoning is kept apart from the answer rather than above it as prose:
  // it is not the answer, and running the two together makes a reasoning
  // model look like it has started rambling.
  const thought = turn.reasoning
    ? `<details class="work thought" data-toggle="thinking" data-index="${index}"${
        thoughtsOpen.has(thoughtKey(index)) ? ' open' : ''}>
         <summary>thinking</summary><div class="md">${markdown(turn.reasoning)}</div></details>`
    : '';
  return `${turnControls(turn, index, busy)}
      ${attachedChips(turn.attachments)}${turn.prompt ? `<div class="said">${escape(turn.prompt)}</div>` : ''}
      ${turnWho(turn)}${thought}${turnActions(turn)}${reply}${turnMeta(turn)}`;
}

/// The markup for every turn, remembered.
///
/// A finished turn is the same string forever, and producing it means running
/// the markdown parser and the syntax colorer over the whole reply. Doing that
/// for every turn on every token is what made a long conversation slow, so
/// each is produced once and kept under a signature of what went into it.
/// The signature also goes on the element as data-sig, which is what lets the
/// patcher skip the subtree without looking inside.
const turnCache = new Map();

/// Turns already on screen, by where they are drawn: "key|signature" of
/// each, read off the page after every draw. A turn here is sent to the
/// patcher as an empty stub with its signature, which the patcher keeps as
/// it is (see morph), instead of its whole markup: a long conversation was
/// otherwise parsed again in full on every token of the reply at its end.
const turnsShown = { transcript: new Set(), 'test-talk': new Set() };

function turnsView(turns, from, where) {
  const busy = !!state.snapshot.busy;
  const shown = turnsShown[where || 'transcript'] || new Set();
  const out = [];
  for (let index = from; index < turns.length; index += 1) {
    const turn = turns[index];
    if (turn.build) {
      // A message that became a build: its journal changes while the build
      // runs and is read later for one that ran, so it is never kept.
      out.push(`<div class="turn build-turn" data-key="t${index}" id="build-${escape(turn.build)}">${
        buildTurnView(turn, index, busy)}</div>`);
      continue;
    }
    if (turn.streaming) {
      // Changing with every push, so not worth remembering.
      out.push(`<div class="turn" data-key="t${index}">${
        turnView(turn, index, busy)}</div>`);
      continue;
    }
    // A revision names the turn word for word; one made by hand has none,
    // and is told apart by what is in it.
    const content = turn.rev ? `r${turn.rev}` : `${(turn.attachments || []).length}|${turn.prompt.length}|${
      turn.reply.length}|${(turn.reasoning || '').length}|${(turn.actions || []).length}|${
      turn.failed}|${turn.canceled}|${turn.output_tokens}`;
    const signature = `${thoughtKey(index)}|${busy}|${thoughtsOpen.has(thoughtKey(index))}|${content}`;
    if (shown.has(`t${index}|${signature}`)) {
      out.push(`<div class="turn" data-key="t${index}" data-sig="${escape(signature)}" data-stub></div>`);
      continue;
    }
    let html = turnCache.get(signature);
    if (html === undefined) {
      html = turnView(turn, index, busy);
      if (turnCache.size > 400) turnCache.clear();
      turnCache.set(signature, html);
    }
    out.push(`<div class="turn" data-key="t${index}" data-sig="${escape(signature)}">${html}</div>`);
  }
  return out.join('');
}

/// After a draw: which turns each transcript now shows. A stub that ended up
/// on the page, because what it stood for was not there after all, is drawn
/// again whole rather than left empty.
afterDraw.push(() => {
  for (const where of Object.keys(turnsShown)) {
    const box = document.getElementById(where);
    const shown = turnsShown[where];
    shown.clear();
    if (!box) continue;
    const stubs = box.querySelectorAll('.turn[data-stub]');
    if (stubs.length) {
      // Taken out first: the patcher keeps an element whose signature
      // matches, and would keep the empty one.
      for (const stub of stubs) stub.remove();
      render();
      continue;
    }
    for (const turn of box.querySelectorAll('.turn[data-sig]')) {
      shown.add(`${turn.getAttribute('data-key')}|${turn.getAttribute('data-sig')}`);
    }
  }
});

// --- nothing to show yet -----------------------------------------------------------

/// Somewhere to start: about the project, when one is open. A chat with no
/// project, or only its scratch folder with nothing in it yet, has no files
/// to ask about, and is offered things that need none, a build among them:
/// the same box makes software as answers questions.
const OPENERS = ['What is in this project?', 'Explain a file to me', 'Find and fix a failing test'];
const SCRATCH_OPENERS = ['Explain something to me', 'Summarize a document', 'Build a small web app'];

/// What Crucible is fetching for itself, while it is: its Python, the
/// runtimes, the training environment. Shown above whatever the empty view
/// says, not instead of it -- a chat works as soon as Python and a runtime are
/// in, and the training environment can take a quarter of an hour. It can be
/// stopped, and a download stopped halfway carries on when it is resumed.
function setupView() {
  const setup = state.snapshot.setup;
  if (!setup || !setup.items) return '';
  const failed = setup.items.filter((i) => i.state === 'failed');
  const stopped = setup.items.some((i) => i.state === 'stopped');
  if (!setup.running && !failed.length && !stopped) return '';
  const rows = setup.items.map((item) => {
    const measured = typeof item.progress === 'number' && item.state === 'working';
    const size = item.download && item.state !== 'done' ? ` (${bytes(item.download)})` : '';
    const said = item.state === 'done' ? 'ready'
               : item.state === 'waiting' ? `waiting${size}`
               : item.detail || item.state;
    return `<div class="setup-item ${escape(item.state)}">
        <div class="setup-line"><span class="setup-name">${escape(item.label)}</span>
          <span class="setup-said">${escape(said)}</span></div>
        ${measured ? `<div class="bar"><span style="width:${Math.round(item.progress * 100)}%"></span></div>` : ''}
      </div>`;
  }).join('');
  const head = setup.running ? 'Setting up' : failed.length ? 'Setup failed' : 'Stopped';
  const button = setup.running ? '<button class="action" data-act="setup-stop">Stop</button>'
               : `<button class="action" data-act="setup-retry">${failed.length ? 'Try again' : 'Resume'}</button>`;
  return `<div class="setup">
      <div class="setup-head">${head}</div>
      ${rows}
      ${button}
    </div>`;
}

actions['setup-retry'] = () => guard(() => call('setup.retry'));
actions['setup-stop'] = () => guard(() => call('setup.stop'));

/// What stands between this window and a first answer, or -- when nothing
/// does -- somewhere to start.
///
/// One thing at a time, in the order they have to be fixed: there is no point
/// saying a model is missing to somebody who has not got a runtime to load
/// it with. Each names the one button that fixes it.
function readiness() {
  const s = state.snapshot;
  const getting = setupView();
  const seated = (s.experts || []).some((e) => e.phase !== 'unconfigured');
  const anyLocal = (s.experts || []).some((e) => e.phase !== 'unconfigured' && !e.provider);
  const runtimes = state.runtimes;
  const noRuntime = runtimes && runtimes.loadable
      && !runtimes.runtimes.some((r) => r.installed);

  let label = '';
  let fix = '';
  const installing = s.setup && s.setup.running;
  if (noRuntime && (anyLocal || !seated) && !installing) {
    // Only when something local wants one. A roster answered entirely by
    // providers needs no runtime, and should not be told to install one.
    label = 'No runtime';
    fix = '<button class="action" data-act="settings-page" data-page="runtimes">Settings</button>';
  } else if (!seated) {
    label = 'No model selected';
    fix = '<button class="action" data-act="settings-page" data-page="experts">Settings</button>';
  }
  if (fix) return getting + `<div class="empty"><div class="empty-label">${label}</div>${fix}</div>`;
  // Nothing in the way: somewhere to start.
  const project = s.project || {};
  const openers = project.open && !project.scratchpad ? OPENERS : SCRATCH_OPENERS;
  return getting + `<div class="empty"><div class="empty-label quiet">Ask anything</div>
      <div class="chips">${openers.map((text) =>
        `<button class="chip" data-act="opener">${escape(text)}</button>`).join('')}</div></div>`;
}

// --- the box ------------------------------------------------------------------------

/// What the conversation has cost and how much room is left in it.
///
/// The second figure is the one that matters: a context that is nearly full
/// is about to start forgetting the beginning, and nothing else says so. So
/// it turns amber before it happens rather than after. Always drawn, zeros
/// and all -- a line that appears after the first reply is a line that moves
/// the box above it.
function tallyView() {
  const s = state.snapshot;
  const used = s.context_size > 0 ? Math.round((s.context_used / s.context_size) * 100) : 0;
  const usage = s.session_usage || {};
  const project = s.project_usage || {};
  const tip = `This conversation: ${usage.input_tokens || 0} tokens read, ${
    usage.output_tokens || 0} written.\nThis project, ever: ${project.input_tokens || 0} read, ${
    project.output_tokens || 0} written.\nContext: ${s.context_used || 0} of ${
    s.context_size || 0} tokens.`;
  return `<div class="tally${used >= 75 ? ' warm' : ''}" title="${escape(tip)}">${
    apart(`${compact(usage.input_tokens)} in`, `${compact(usage.output_tokens)} out`, `${used}% context used`)}</div>`;
}

// --- who answers, and how hard it thinks -----------------------------------------------
//
// Two menus beside the plus, opening upward like it. The first is where a
// prompt goes: to the delegator, which picks an expert for each one, or
// straight to one expert -- what `/physics` does for a single prompt, kept
// until it is changed back; a build sent to one is that expert's to plan and
// work. The second is how hard a reasoning model thinks in a chat turn; a
// model with no such setting is never sent it, and a build's rounds are sent
// none (engine_cook.cpp, cook_round).

const PICK = {
  route: `<svg viewBox="0 0 16 16" width="14" height="14" aria-hidden="true" fill="none" stroke="currentColor"
    stroke-width="1.3" stroke-linecap="round" stroke-linejoin="round"><circle cx="3.5" cy="8" r="1.6"/>
    <circle cx="12.5" cy="3.5" r="1.6"/><circle cx="12.5" cy="12.5" r="1.6"/>
    <path d="M5 8h2.5c1.4 0 1.6-4.5 3.4-4.5M7.5 8c1.4 0 1.6 4.5 3.4 4.5"/></svg>`,
  seat: '<svg viewBox="0 0 16 16" width="12" height="12" aria-hidden="true"><path d="M8 2.5 13.5 8 8 13.5 2.5 8z" fill="currentColor"/></svg>',
  effort: `<svg viewBox="0 0 16 16" width="14" height="14" aria-hidden="true" fill="none" stroke="currentColor"
    stroke-width="1.3" stroke-linecap="round"><path d="M2.8 11.5a5.6 5.6 0 1 1 10.4 0"/><path d="M8 8.6l2.6-2.8"/></svg>`,
  chevron: `<svg class="chev" viewBox="0 0 16 16" width="11" height="11" aria-hidden="true" fill="none"
    stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"><path d="M4 6l4 4 4-4"/></svg>`,
  check: `<svg viewBox="0 0 16 16" width="13" height="13" aria-hidden="true" fill="none" stroke="currentColor"
    stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"><path d="M3.5 8.5l3 3 6-7"/></svg>`,
  up: `<svg viewBox="0 0 16 16" width="15" height="15" aria-hidden="true" fill="none" stroke="currentColor"
    stroke-width="1.9" stroke-linecap="round" stroke-linejoin="round"><path d="M8 13V3.5M3.8 7.5 8 3.3l4.2 4.2"/></svg>`,
  stop: '<svg viewBox="0 0 16 16" width="12" height="12" aria-hidden="true"><rect x="3.5" y="3.5" width="9" height="9" rx="1.5" fill="currentColor"/></svg>',
};

const EFFORTS = [['', 'Default'], ['low', 'Low'], ['medium', 'Medium'], ['high', 'High']];

/// The experts a prompt can be sent to straight: every seat with a model.
function pickableExperts() {
  return (state.snapshot.experts || []).filter((e) => e.phase !== 'unconfigured');
}

/// Where prompts go: an expert's id, or '' for the delegator. An expert that
/// has since lost its model, or its seat, sends them back to the delegator.
function routeTarget() {
  const id = state.route || '';
  return id && pickableExperts().some((e) => e.id === id) ? id : '';
}

function pickerButton(which, mode, icon, label, title, disabled) {
  const open = state.menu === `${which}:${mode}` && !disabled;
  return `<button type="button" class="picker" data-act="${which}-menu" aria-haspopup="menu"
      aria-expanded="${open}"${title ? ` title="${escape(title)}"` : ''} ${disabled ? 'disabled' : ''}>${icon}<span class="label">${
      escape(label)}</span>${PICK.chevron}</button>`;
}

function menuItem(act, value, label, hint, chosen, extra) {
  return `<button type="button" role="menuitemradio" aria-checked="${chosen}" data-act="${act}"
      data-value="${escape(value)}">${extra || ''}<span class="mi-label">${escape(label)}</span>${
      hint ? `<span class="mi-hint">${escape(hint)}</span>` : ''}<span class="mi-check">${chosen ? PICK.check : ''}</span></button>`;
}

function routePicker(mode, disabled) {
  const target = routeTarget();
  const label = target ? expertName(target) : 'Delegator';
  const open = state.menu === `route:${mode}` && !disabled;
  const experts = pickableExperts();
  const menu = open ? `<div class="popmenu picks" role="menu">
      ${menuItem('route-pick', '', 'Delegator', '', !target, PICK.route)}
      <div class="mi-caption">EXPERTS</div>
      ${experts.length ? experts.map((e) => menuItem('route-pick', e.id, e.name, '', target === e.id,
          `<span class="mi-seat">${e.provider ? ICONS.cloud : PICK.seat}</span>`)).join('')
        : '<div class="mi-empty">None with a model.</div>'}
    </div>` : '';
  return `<div class="menu-wrap">${pickerButton('route', mode, target ? PICK.seat : PICK.route, label,
      target ? `Everything goes to ${label}` : 'The delegator picks the expert',
      disabled)}${menu}</div>`;
}

function effortPicker(mode, disabled) {
  const current = state.snapshot.reasoning_effort || '';
  const chosen = EFFORTS.find((e) => e[0] === current) || EFFORTS[0];
  const open = state.menu === `effort:${mode}` && !disabled;
  const menu = open ? `<div class="popmenu picks" role="menu">
      <div class="mi-caption">REASONING EFFORT</div>
      ${EFFORTS.map(([value, label]) => menuItem('effort-pick', value, label, '', value === current)).join('')}
      <div class="mi-note">Reasoning models only</div>
    </div>` : '';
  return `<div class="menu-wrap">${pickerButton('effort', mode, PICK.effort, `${chosen[1]} effort`,
      '', disabled)}${menu}</div>`;
}

actions['route-menu']  = () => toggleMenu('route');
actions['effort-menu'] = () => toggleMenu('effort');
actions['route-pick'] = (item) => {
  state.route = item.dataset.value || '';
  remember.set('route', state.route);
  state.menu = null;
  render();
  // Loaded now, not when the first prompt to it is sent.
  call('route.pick', { expert: routeTarget(), load: true }).catch(() => {});
};

actions['effort-pick'] = (item) => {
  state.menu = null;
  // Said at once; the setting is written behind it.
  state.snapshot.reasoning_effort = item.dataset.value || '';
  configure({ reasoning_effort: item.dataset.value || '' });
};

/// The box everything is typed into: a question, something to build, or the
/// answer to what a build is asking.
///
/// One box, because what a message becomes is decided rather than chosen
/// (the orchestrator's intent.py): a question is answered, a request for
/// software becomes a build drawn in the conversation. While a build asks
/// something, the box is for the answer. Otherwise it is never shut by one:
/// a message typed while a build works is answered beside its agents, by the
/// same experts, one model at a time each.
function composerView() {
  const s = state.snapshot;
  const cook = s.cook && s.cook.running ? s.cook : null;
  const asking = !!cook && cook.state === 'asking';
  const finishing = !!cook && cook.state === 'finishing';
  const auto_ = !!s.auto_edits;
  const hint = asking ? 'answer the question above' : 'Ask or build anything';

  // Auto sits beside the box rather than in Settings because whether you are
  // watching an expert edit your files is a decision that changes between one
  // prompt and the next, and a switch you have to go and find is a switch that
  // stays wherever it was last left. A build asks too, when it is off, and the
  // switch takes effect from its next write -- so it is here while one runs.
  const autoButton = `<button type="button" class="action toggle" data-act="auto-edits"
      aria-pressed="${auto_}">${auto_ ? 'Auto on' : 'Auto off'}</button>`;

  // A build's two stops stay at hand while it runs, whichever pane is up,
  // because they are different things: the first makes a finishing pass so
  // the project is left in a state that runs, the second is a cancel and
  // leaves it wherever it got to.
  const stops = cook ? `${finishing ? '<span class="status composer-note">finishing up</span>' : ''}
      <button type="button" class="action" data-act="cook-stop" ${finishing ? 'disabled' : ''}
              title="Finish the build cleanly, then stop">Stop and finish</button>
      <button type="button" class="action" data-act="build-cancel" title="Stop the build at once">Stop now</button>` : '';
  const send = asking ? 'Answer' : 'Send';
  const buttons = `${stops}${autoButton}
      ${s.busy && !asking
        ? `<button type="button" class="send stop" data-act="stop" aria-label="${
             s.status === 'stopping' ? 'Stopping' : 'Stop'}">${PICK.stop}</button>`
        : `<button class="send" aria-label="${escape(send)}">${PICK.up}</button>`}`;

  // One rounded box: what is attached, then the text, then a row along the
  // bottom with the plus at one end and the buttons at the other. More can
  // be typed with tiles in it; they go with whatever is.
  return `<div class="foot">
      <div class="splitter-y" id="composer-splitter" title="Drag to resize"></div>
      <form class="composer" data-submit="send" data-mode="chat">
        <div class="box">
          ${asking ? '' : attachTiles('chat')}
          <textarea id="prompt" data-draft data-sized data-key="composer-key" data-input="composer-grow"
                    data-paste="composer-paste"
                    rows="${state.composerRows}" placeholder="${escape(hint)}" autocomplete="off"
                    spellcheck="false">${escape(state.drafts.chat || '')}</textarea>
          <div class="box-bar">
            ${attachButton('chat', asking)}
            ${routePicker('chat', asking)}
            ${effortPicker('chat', asking)}
            <span class="spacer"></span>
            ${buttons}
          </div>
        </div>
      </form>
      ${tallyView()}
    </div>`;
}

// --- the view -------------------------------------------------------------------------

/// The conversation: every turn, those that became builds drawn as their
/// builds, or somewhere to start when there are none.
///
/// No notices here: the transcript is the conversation and nothing else.
/// What the program has to say goes to the side menu's status line, and a
/// seat that would not load says so on its own row.
function conversationView() {
  const s = state.snapshot;
  const turns = s.turns || [];
  return (turns.length ? turnsView(turns, 0) : readiness()) + (s.pending_edit ? pendingEdit(s.pending_edit) : '');
}

views.chat = () => {
  // The panes beside the conversation, once there is a folder to show (see
  // build.js). The switcher sits above the scroller, not inside it: a long
  // conversation scrolled it out of reach. Only the conversation keeps to
  // its bottom as it grows; the other panes are read from the top, and a
  // tree or a preview toolbar scrolled out of sight is a pane with nothing
  // to click.
  const pane = shownPane();
  const nav = paneNav();
  const body = pane === 'code' ? codePane() : pane === 'source' ? sourcePane()
             : pane === 'preview' ? previewView() : conversationView();
  return sideView() + `<div class="pane${nav ? ' with-nav' : ''}">${nav}
      <div class="scroller"><div id="${pane === 'chat' ? 'transcript' : 'pane-body'}">${body}</div>
        <button class="jump" id="jump" data-act="jump" hidden>${ICONS.down} Jump to latest</button></div>
      ${composerView()}
    </div>` + recentsView();
};

// --- what a person can do here ------------------------------------------------------------

/// What is typed, kept by the page rather than only by the box: a box is
/// redrawn new whenever the view changes, and with it went whatever was
/// half written.
function keepDraft(text) { state.drafts.chat = text; }

/// Clear the box for a send, and put the words back if the send failed.
async function sending(box, text, work) {
  box.value = '';
  keepDraft('');
  growComposer(box);
  try {
    await work();
  } catch (error) {
    keepDraft(text);
    const now = document.getElementById('prompt');
    if (now && !now.value) { now.value = text; growComposer(now); }
    throw error;
  }
}

actions.send = (form) => {
  const box = form.querySelector('textarea');
  const text = box.value.trim();
  const s = state.snapshot;
  // While a build asks something, what is typed is the answer to it: the
  // question is on screen, and the box under it answers it. What is
  // attached stays where it is for the next prompt.
  if (s.cook && s.cook.running && s.cook.state === 'asking') {
    if (!text) return;
    state.follow = true;
    return guard(() => sending(box, text, () => call('cook.answer', { answer: text })));
  }
  if (s.busy) return;
  // A question can be only its attachments: "here" and a PDF says enough.
  if (!text && !(state.attached.chat || []).length) return;
  const attachments = attachmentsFor('chat');
  if (!attachments) {
    state.error = 'Still reading the attachments';
    return render();
  }
  state.follow = true;
  // A build this becomes opens on whichever of its agents is at work.
  state.open.task = null;
  state.open.taskOf = '';
  return guard(() => sending(box, text, async () => {
    // A slash and a name for this prompt wins over the menu's choice.
    const asked = pinned(text);
    if (!asked.expert && routeTarget()) asked.expert = routeTarget();
    // The menu's choice goes too, slash or not: it is what stays loaded.
    await call('submit', Object.assign(asked, { attachments, route: routeTarget() }));
    state.attached.chat = [];
  }));
};

/// "/physics why is the sky blue" sends the question straight to Physics.
///
/// The delegator is skipped, which is the point: sometimes you know who
/// should answer, and a slash and a name is quicker than hoping it agrees.
/// Anything that is not the name of a seat is a prompt like any other -- a
/// question that happens to begin with a path is still a question.
function pinned(text) {
  const match = /^\/([^\s/]+)\s+([\s\S]+)$/.exec(text);
  if (match) {
    const wanted = match[1].toLowerCase();
    const seat = (state.snapshot.experts || []).find((e) =>
      e.id.toLowerCase() === wanted || e.name.toLowerCase().replace(/\s+/g, '-') === wanted
      || (e.tag || '').toLowerCase() === wanted);
    if (seat) return { prompt: match[2], expert: seat.id };
  }
  return { prompt: text };
}

/// Enter sends and Shift+Enter is a new line, which is what a chat box does
/// everywhere else. Not while an input method is composing: Enter there
/// chooses a character.
actions['composer-key'] = (box, event) => {
  if (event.key === 'Enter' && !event.shiftKey && !event.isComposing) {
    event.preventDefault();
    box.form.requestSubmit();
  }
};

/// Grow the box to fit what is in it, up to a third of the window, and never
/// below the rows the splitter asked for.
function growComposer(box) {
  box.style.height = 'auto';
  const asked = box.offsetHeight;
  const fits = box.scrollHeight + box.offsetHeight - box.clientHeight;
  box.style.height = `${Math.max(asked, Math.min(fits, window.innerHeight / 3))}px`;
}
actions['composer-grow'] = (box) => {
  keepDraft(box.value);
  growComposer(box);
};

actions.opener = (chip) => {
  const box = document.getElementById('prompt');
  if (!box || box.disabled) return;
  box.value = chip.textContent;
  keepDraft(box.value);
  growComposer(box);
  box.focus();
};

actions.stop = () => {
  // Said at once. Stopping a model mid-thought can take a moment, and a
  // button that does nothing visible gets pressed again. The chat's turn
  // only: a build running beside it has two stops of its own.
  state.snapshot.status = 'stopping';
  guard(() => call('cancel'));
};
actions['build-cancel'] = () => guard(() => call('cook.cancel'));
actions['turn-retry']  = (e) => guard(() => call('turn.retry',  { index: Number(e.dataset.index) }));
actions['turn-delete'] = (e) => guard(() => call('turn.delete', { index: Number(e.dataset.index) }));
actions['edit-allow']  = () => guard(() => call('edit.approve', { approved: true }));
actions['edit-deny']   = () => guard(() => call('edit.approve', { approved: false }));
actions['auto-edits']  = () => configure({ tools: { auto_edits: !state.snapshot.auto_edits } });

/// Opening or closing a turn's thinking, remembered so a redraw leaves it so.
actions.thinking = (details) => {
  const key = thoughtKey(Number(details.dataset.index));
  if (details.open === thoughtsOpen.has(key)) return;
  if (details.open) thoughtsOpen.add(key); else thoughtsOpen.delete(key);
  render();
};

/// Copy a code block. The text is read back out of the document rather than
/// kept beside it, so what is copied is exactly what is shown.
actions.copy = (button) => {
  const code = button.closest('.code').querySelector('.code-text');
  if (!code) return;
  const done = () => {
    button.classList.add('done');
    setTimeout(() => button.classList.remove('done'), 1200);
  };
  if (navigator.clipboard && navigator.clipboard.writeText) {
    navigator.clipboard.writeText(code.textContent).then(done, () => {});
  }
};

actions.jump = () => { keepAtBottom('transcript', true); showJump(); };

/// The way back down, offered to somebody who has scrolled up away from it.
function showJump() {
  const transcript = document.getElementById('transcript');
  const jump = document.getElementById('jump');
  if (transcript && jump) jump.hidden = following.transcript || fromBottom(transcript) < 40;
}

// --- after each draw -------------------------------------------------------------------------

/// Size the box when it is new -- another view's, or a draft come back --
/// and when the splitter has changed how many rows it starts at. Typing
/// sizes it as it goes (composer-grow).
let sizedBox = null;
let sizedRows = 0;
afterDraw.push(() => {
  const box = document.getElementById('prompt');
  if (!box || (box === sizedBox && state.composerRows === sizedRows)) return;
  sizedBox = box;
  sizedRows = state.composerRows;
  growComposer(box);
});

/// Follow the bottom of the transcript, but only for somebody who is there.
///
/// Scrolling up to read something is a decision, and a reply that is still
/// arriving should not undo it. So the view stays where it was put, and a
/// button offers the way back down instead.
const jumpWatched = new WeakSet();
afterDraw.push(() => {
  const transcript = keepAtBottom('transcript', state.follow);
  state.follow = false;
  if (!transcript) return;
  showJump();
  if (!jumpWatched.has(transcript)) {
    jumpWatched.add(transcript);
    transcript.addEventListener('scroll', showJump, { passive: true });
  }
});

/// Put the caret somewhere useful once, when the thing it belongs in appears:
/// the first field of a dialog, the box under the transcript.
let focused = null;
afterDraw.push(() => {
  const wanted = document.querySelector('#layer [data-focus]')
      || (state.modal ? null : document.querySelector('textarea.code-editor')
                               || document.querySelector('#prompt:not([disabled])'));
  if (wanted && wanted !== focused && !document.querySelector('#layer :focus')) {
    // Not out from under somebody typing elsewhere on the page.
    const active = document.activeElement;
    const typing = active && active !== document.body && active !== wanted
        && /^(INPUT|TEXTAREA|SELECT)$/.test(active.tagName);
    if (!typing || wanted.closest('#layer')) wanted.focus();
  }
  focused = wanted || null;
});
