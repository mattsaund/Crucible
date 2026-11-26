// SPDX-License-Identifier: MIT
//
// The conversation: what was asked, who answered and how, and the one box
// the next message goes in.

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

/// Who has the turn still arriving: the delegator while it chooses, then the
/// expert it chose, in the program's color above where the reply will be,
/// with a model's load as a ring beside the name while it loads. Read from
/// the program's state rather than the turn's, since an expert can hand the
/// turn to another partway through.
function liveWho(turn) {
  const s = state.snapshot;
  const id = s.mood === 'routing' ? '' : s.linked || (turn.route && turn.route.expert) || '';
  const expert = id ? (s.experts || []).find((e) => e.id === id) : null;
  if (!expert) {
    const problem = s.delegator_problem || '';
    const tip = problem ? `${(s.delegator || {}).model}: ${problem}\nRouting on keywords` : 'Choosing an expert';
    // Its load as a figure while it loads, and turning while it chooses.
    return `<div class="who live" title="${escape(tip)}"><span class="seat">◆ Delegator</span>${
      ring(s.delegator_progress !== undefined ? s.delegator_progress : -1)}</div>`;
  }
  return `<div class="who live" title="${escape(expert.model || '')}"><span class="seat">◆ ${escape(expert.name)}</span>${
    expert.phase === 'loading' ? ring(expert.progress) : ''}${
    expert.provider ? `<span>answered by ${escape(expert.provider)}</span>` : ''}</div>`;
}

/// How long since `since` (milliseconds since the epoch), as the line beside
/// an activity says it: "12s", "3 min".
function elapsed(since) {
  return since ? span(Math.max(0, Date.now() - since) / 1000) : '';
}

/// The most of a file being written or a command's output shown under the
/// activity: its last lines, the ones arriving.
const DOING_LINES = 14;

/// What the turn still arriving is doing this moment, at its foot, where the
/// next thing it does will appear: reading the prompt and how far through,
/// thinking and for how long, a file being written with how much of it
/// there is and its last lines as they come, a command with what it has
/// printed. A model writing a page through a tool, or a script making a PDF,
/// showed nothing at all for minutes before this. From the engine's state, as
/// liveWho is: see Activity in engine/state.hpp.
function liveActivity() {
  const a = state.snapshot.activity;
  if (!a || !state.snapshot.busy) return '';
  const numbers = [];
  if (typeof a.progress === 'number' && a.progress >= 0) numbers.push(`${Math.round(a.progress * 100)}%`);
  if (a.kind === 'write' && a.bytes) numbers.push(bytes(a.bytes), count(a.lines || 0, 'line'));
  else if (a.kind === 'run' && a.lines) numbers.push(count(a.lines, 'line'));
  const head = `<div class="doing" data-kind="${escape(a.kind || '')}">
      <span class="spin" aria-hidden="true"></span>
      <span class="doing-label">${escape(a.label || '')}</span>${
      a.detail ? `<span class="doing-detail" title="${escape(a.detail)}">${escape(a.detail)}</span>` : ''}${
      numbers.map((n) => `<span class="doing-num">${escape(n)}</span>`).join('')}
      <span class="elapsed" data-since="${Number(a.since) || 0}">${elapsed(a.since)}</span></div>`;
  if (!a.tail) return head;
  const lines = String(a.tail).split('\n');
  if (lines.length && lines[lines.length - 1] === '') lines.pop();
  const shown = lines.slice(-DOING_LINES).join('\n');
  const lang = a.kind === 'run' ? 'shell' : languageOf(a.language || '') || a.language || '';
  return `${head}<div class="doing-tail${a.kind === 'run' ? ' output' : ''}"><pre><code>${
    a.kind === 'run' ? escape(shown) : highlight(shown, lang)}</code></pre></div>`;
}

/// The times beside an activity, kept counting between pushes: a command
/// that prints nothing for a minute brings no push to redraw them. Written
/// straight into the elements, once a second while something runs.
if (typeof setInterval === 'function') {
  setInterval(() => {
    if (!state.snapshot.busy || typeof document.querySelectorAll !== 'function') return;
    for (const clock of document.querySelectorAll('.elapsed[data-since]')) {
      const text = elapsed(Number(clock.getAttribute('data-since')));
      if (clock.textContent !== text) clock.textContent = text;
    }
  }, 1000);
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
  return (turn.actions || []).map(actionView).join('');
}

/// Each action's markup, by what it is made from. A turn still arriving is
/// drawn again for every push, and every file it had written with it, colored
/// and matched line by line each time: a long page written early in a turn
/// was most of what each push of the rest of it cost. Signed as well, so the
/// patcher passes over one that is on screen already.
const actionsMade = new Map();
let actionSerial = 0;
function actionView(a) {
  const key = `${a.summary}\u0000${a.language || ''}\u0000${a.picture || ''}\u0000${a.body || ''}`;
  let html = actionsMade.get(key);
  if (html === undefined) {
    if (actionsMade.size > 400) actionsMade.clear();
    html = `<div class="act" data-sig="act${++actionSerial}">${
      escape(a.summary)}${pictureChip(a.picture)}${actionBody(a)}</div>`;
    actionsMade.set(key, html);
  }
  return html;
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

/// One exchange, as markup. `where` is the transcript a turn still arriving
/// is drawn in, which its blocks are kept by (see streamedMarkdown).
function turnView(turn, index, busy, where) {
  const live = turn.streaming && where;
  let reply;
  if (turn.failed) {
    reply = `<div class="body failed">${escape(turn.reply || 'it could not answer')}</div>`;
  } else if (!turn.reply && turn.streaming) {
    // Nothing yet: the fold of its thinking, below, says so.
    reply = '';
  } else if (live) {
    reply = `<div class="body md" id="${where}-reply">${streamedMarkdown(turn.reply, `${where}-reply`)}</div>`;
  } else {
    reply = `<div class="body md">${markdown(turn.reply)}</div>`;
  }
  // Reasoning is kept apart from the answer rather than above it as prose:
  // it is not the answer, and running the two together makes a reasoning
  // model look like it has started rambling. What it says is drawn only
  // while it is open: a reasoning model thinks at length, and all of it was
  // drawn again on every push behind a closed fold.
  // A turn still arriving has the fold from its first moment, before the
  // delegator has chosen or the model has said a word, so it can be opened
  // early: dots in it until the thinking starts. A model that answers
  // without thinking loses it with the first word of the answer.
  const open = thoughtsOpen.has(thoughtKey(index));
  const early = turn.streaming && !turn.reasoning && !turn.reply;
  const thinking = !open ? '' : early ? '<span class="waiting">...</span>'
    : live ? streamedMarkdown(turn.reasoning, `${where}-thought`) : markdown(turn.reasoning);
  const thought = turn.reasoning || early
    ? `<details class="work thought" data-toggle="thinking" data-index="${index}"${open ? ' open' : ''}>
         <summary>thinking</summary><div class="md"${live && open ? ` id="${where}-thought"` : ''}>${
           thinking}</div></details>`
    : '';
  return `${turnControls(turn, index, busy)}
      ${attachedChips(turn.attachments)}${turn.prompt ? `<div class="said">${escape(turn.prompt)}</div>` : ''}
      ${turn.streaming ? liveWho(turn) : turnWho(turn)}${thought}${turnActions(turn)}${reply}${
      live ? liveActivity() : ''}${turnMeta(turn)}`;
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

/// How much of a conversation is drawn at once: its newest turns, back until
/// they hold about this many characters -- several screens -- or are this
/// many turns. Those before them are drawn as the reader scrolls up to them,
/// so opening a long conversation, or coming back to Chat, costs what is
/// near its end rather than all of it: three hundred turns took seconds.
const DRAWN_AT_ONCE = { chars: 40000, turns: 40 };

/// Where a stretch of `turns` drawn at once starts, when it ends before `end`.
function drawnFrom(turns, end) {
  // Never past the end: a scroll can land between a conversation getting
  // shorter and the draw that notices.
  let from = Math.min(end, turns.length);
  const last = from;
  let chars = 0;
  while (from > 0 && last - from < DRAWN_AT_ONCE.turns && chars < DRAWN_AT_ONCE.chars) {
    from -= 1;
    chars += (turns[from].prompt || '').length + (turns[from].reply || '').length;
  }
  return from;
}

/// The first turn the transcript draws, and the conversation that is of.
const transcriptWindow = { session: null, from: 0, growing: null };

function transcriptStart(turns) {
  const w = transcriptWindow;
  const session = state.snapshot.session || '';
  if (w.session !== session || w.from > turns.length) {
    w.session = session;
    w.from = drawnFrom(turns, turns.length);
  }
  return w.from;
}

function turnsView(turns, from, where) {
  const busy = !!state.snapshot.busy;
  const at = where || 'transcript';
  const shown = turnsShown[at] || new Set();
  const first = at === 'transcript' ? Math.max(from, transcriptStart(turns)) : from;
  // The names above the replies are the roster's, so a renamed expert is a
  // different drawing of every turn it answered.
  const names = (state.snapshot.experts || []).map((e) => `${e.id}=${e.name}`).join(',');
  const out = [];
  for (let index = first; index < turns.length; index += 1) {
    const turn = turns[index];
    if (turn.streaming) {
      // Changing with every push, so drawn a block at a time instead.
      out.push(`<div class="turn" data-key="t${index}">${
        turnView(turn, index, busy, at)}</div>`);
      continue;
    }
    // A revision names the turn word for word; one made by hand has none,
    // and is told apart by what is in it.
    const content = turn.rev ? `r${turn.rev}` : `${(turn.attachments || []).length}|${turn.prompt.length}|${
      turn.reply.length}|${(turn.reasoning || '').length}|${(turn.actions || []).length}|${
      turn.failed}|${turn.canceled}|${turn.output_tokens}`;
    const signature = `${thoughtKey(index)}|${busy}|${thoughtsOpen.has(thoughtKey(index))}|${content}|${names}`;
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

/// After a draw: what each element named in `shown` -- a transcript, a
/// reply arriving -- now shows of its own children, as "key|signature" in
/// `shown[id]`. A stub that ended up on the page, because what it stood for
/// was not there after all, is drawn again whole rather than left empty.
/// Only the children are read: searching every element of every reply in a
/// long conversation was the slowest thing a draw did.
function readShown(shown) {
  for (const id of Object.keys(shown)) {
    const box = document.getElementById(id);
    const seen = shown[id];
    seen.clear();
    if (!box) continue;
    const children = Array.from(box.children);
    const stubs = children.filter((child) => child.hasAttribute('data-stub'));
    if (stubs.length) {
      // Taken out first: the patcher keeps an element whose signature
      // matches, and would keep the empty one.
      for (const stub of stubs) stub.remove();
      render();
      continue;
    }
    for (const child of children) {
      const sig = child.getAttribute('data-sig');
      if (sig !== null) seen.add(`${child.getAttribute('data-key')}|${sig}`);
    }
  }
}
afterDraw.push(() => readShown(turnsShown));

// --- a reply still arriving ---------------------------------------------------------

/// The finished blocks of the replies arriving, each one's markup and its
/// signature by its source; and the block still arriving, by where it is
/// drawn. Kept until there are a few hundred, which is one very long reply.
const streamMade = new Map();
const streamSigs = new Map();
const streamTails = {};
let streamSerial = 0;

/// The blocks on screen of each reply arriving, by the id of what they are
/// drawn in: see readShown.
const blocksShown = {};

/// `text`, still arriving, as markup drawn in the element `id`: its blocks,
/// each signed, and those on screen already as their empty elements, which
/// the patcher keeps as they are (see morph). What arrives next can only
/// change the last block, so every other one is made once and parsed once:
/// a long reply was made and parsed again in full for every few words.
function streamedMarkdown(text, id) {
  if (streamMade.size > 400) { streamMade.clear(); streamSigs.clear(); }
  const blocks = markdownBlocks(text, streamMade);
  const shown = blocksShown[id] || (blocksShown[id] = new Set());
  return blocks.map((block, n) => {
    let sig;
    if (n < blocks.length - 1) {
      sig = streamSigs.get(block.source);
      if (sig === undefined) streamSigs.set(block.source, sig = `b${++streamSerial}`);
    } else {
      const tail = streamTails[id];
      if (!tail || tail.source !== block.source) streamTails[id] = { source: block.source, sig: `b${++streamSerial}` };
      sig = streamTails[id].sig;
    }
    // Every block is one element, and opens with it.
    const tag = /^<([a-z][a-z0-9]*)/.exec(block.html)[1];
    const signed = `<${tag} data-key="b${n}" data-sig="${sig}"`;
    if (shown.has(`b${n}|${sig}`)) return `${signed} data-stub>${tag === 'hr' ? '' : `</${tag}>`}`;
    return signed + block.html.slice(tag.length + 1);
  }).join('');
}
afterDraw.push(() => readShown(blocksShown));

// --- nothing to show yet -----------------------------------------------------------

/// Somewhere to start: about the project, when one is open. A chat with no
/// project, or only its scratch folder with nothing in it yet, has no files
/// to ask about, and is offered things that need none, a small app among
/// them: the same box makes software as answers questions.
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
// Two menus beside the plus, opening upward like it, and a switch. The first
// menu is where a prompt goes: to the delegator, which picks an expert for
// each one, or straight to one expert, as `/physics` does for a single
// prompt, kept until it is changed back. The second is how hard a reasoning
// model thinks, and the switch whether it thinks at all.
//
// Both follow the model. With an expert chosen they offer exactly what its
// model takes (see llm/reasoning.hpp): GPT OSS's three levels, Claude's five,
// none for a model with no effort to set, and a switch only where thinking
// can be switched, held on where it cannot be turned off. What is chosen is
// that expert's own. With the delegator, they set the box's, which every
// expert without one of its own is given as the nearest level it takes.

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
  think: `<svg viewBox="0 0 16 16" width="14" height="14" aria-hidden="true" fill="none" stroke="currentColor"
    stroke-width="1.3" stroke-linecap="round" stroke-linejoin="round"><path d="M6 12.5h4M6.5 14.5h3"/>
    <path d="M5.6 10.5C4.3 9.6 3.5 8.3 3.5 6.8a4.5 4.5 0 0 1 9 0c0 1.5-.8 2.8-2.1 3.7"/></svg>`,
  unthink: `<svg viewBox="0 0 16 16" width="14" height="14" aria-hidden="true" fill="none" stroke="currentColor"
    stroke-width="1.3" stroke-linecap="round" stroke-linejoin="round"><path d="M6 12.5h4M6.5 14.5h3"/>
    <path d="M5.6 10.5C4.3 9.6 3.5 8.3 3.5 6.8a4.5 4.5 0 0 1 9 0c0 1.5-.8 2.8-2.1 3.7"/><path d="M2.5 2.5l11 11"/></svg>`,
};

/// The words on the effort scale, as the menu says them, lowest first: what
/// the core calls reasoning::scale().
const EFFORT_NAMES = { minimal: 'Minimal', low: 'Low', medium: 'Medium', high: 'High', xhigh: 'Extra high', max: 'Max' };
const SCALE = Object.keys(EFFORT_NAMES);

/// What each expert's model takes, by expert and model: a reasoning.profile
/// answer, or 'asking' while the question is out. A change of model asks
/// again, since it is a different model.
const profiles = {};

function profileKey(id) {
  const seat = (state.snapshot.experts || []).find((e) => e.id === id) || {};
  return `${id}|${seat.provider || ''}|${seat.model || ''}`;
}

/// The profile of `id`'s model, or null while it is asked for. The box's
/// own, for the delegator, is the whole scale and a switch.
function profileOf(id) {
  if (!id) return { levels: SCALE, thinking: 'optional', thinks_unasked: true, standard: '', source: 'box' };
  const key = profileKey(id);
  const known = profiles[key];
  if (known && known !== 'asking') return known;
  if (!known) {
    profiles[key] = 'asking';
    call('reasoning.profile', { expert: id })
      .then((got) => { profiles[key] = got; })
      .catch(() => { profiles[key] = { levels: [], thinking: 'none', standard: '' }; })
      .finally(render);
  }
  return null;
}

/// What an expert has set of its own, from the config: its effort and its
/// thinking, each empty for "as the box says".
function seatSetting(id) {
  const seat = ((state.config && state.config.experts) || []).find((e) => e.id === id) || {};
  return { effort: seat.effort || '', thinking: seat.thinking || '' };
}

/// The level `wanted` comes to on a model that takes `levels`: what the core's
/// reasoning::nearest does, so the menu says what will be sent.
function nearestLevel(levels, wanted) {
  if (!wanted || !levels || !levels.length) return '';
  if (levels.includes(wanted)) return wanted;
  const asked = SCALE.indexOf(wanted);
  if (asked < 0) return '';
  let best = '';
  let gap = Infinity;
  for (const level of levels) {
    const distance = Math.abs(SCALE.indexOf(level) - asked);
    if (distance < gap || (distance === gap && SCALE.indexOf(level) > SCALE.indexOf(best))) {
      gap = distance;
      best = level;
    }
  }
  return best;
}

/// Whether the model with `profile` thinks, as `id` is set: its own switch,
/// else the box's, else what it does unasked.
function thinkingOn(id, profile) {
  if (!profile || profile.thinking === 'none') return false;
  if (profile.thinking === 'always') return true;
  const own = id ? seatSetting(id).thinking : '';
  const set = own || state.snapshot.thinking || '';
  return set ? set === 'on' : profile.thinks_unasked !== false;
}

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

function pickerButton(which, mode, icon, label, title, trouble) {
  const open = state.menu === `${which}:${mode}`;
  return `<button type="button" class="picker${trouble ? ' trouble' : ''}" data-act="${which}-menu" aria-haspopup="menu"
      aria-expanded="${open}"${title ? ` title="${escape(title)}"` : ''}>${icon}<span class="label">${
      escape(label)}</span>${PICK.chevron}</button>`;
}

function menuItem(act, value, label, hint, chosen, extra) {
  return `<button type="button" role="menuitemradio" aria-checked="${chosen}" data-act="${act}"
      data-value="${escape(value)}">${extra || ''}<span class="mi-label">${escape(label)}</span>${
      hint ? `<span class="mi-hint">${escape(hint)}</span>` : ''}<span class="mi-check">${chosen ? PICK.check : ''}</span></button>`;
}

function routePicker(mode) {
  const target = routeTarget();
  const label = target ? expertName(target) : 'Delegator';
  const open = state.menu === `route:${mode}`;
  const experts = pickableExperts();
  // A delegator that would not load is said here, on the one thing that
  // names it: prompts go out on keywords until it does.
  const problem = state.snapshot.delegator_problem || '';
  const trouble = problem ? `${(state.snapshot.delegator || {}).model}: ${problem}\nRouting on keywords` : '';
  const menu = open ? `<div class="popmenu picks" role="menu">
      ${menuItem('route-pick', '', 'Delegator', '', !target, PICK.route)}
      ${trouble ? `<div class="mi-note bad">${escape(trouble)}</div>` : ''}
      <div class="mi-caption">EXPERTS</div>
      ${experts.length ? experts.map((e) => menuItem('route-pick', e.id, e.name, '', target === e.id,
          `<span class="mi-seat">${e.provider ? ICONS.cloud : PICK.seat}</span>`)).join('')
        : '<div class="mi-empty">None with a model.</div>'}
    </div>` : '';
  return `<div class="menu-wrap">${pickerButton('route', mode, target ? PICK.seat : PICK.route, label,
      target ? `Everything goes to ${label}` : trouble || 'The delegator picks the expert', !target && !!trouble)}${menu}</div>`;
}

/// How hard it thinks: the levels the chosen expert's model takes, or with
/// the delegator, the whole scale for the box's own setting. Nothing for a
/// model that takes no effort.
function effortPicker(mode) {
  const target = routeTarget();
  const profile = profileOf(target);
  const levels = profile ? profile.levels || [] : [];
  if (target && profile && !levels.length) return '';
  const box = state.snapshot.reasoning_effort || '';
  // The expert's own, when it has one; the box's otherwise, which it gets
  // as the nearest level it takes.
  const own = target ? seatSetting(target).effort : box;
  const standard = (profile && profile.standard) || '';
  const fallback = (target && nearestLevel(levels, box)) || standard;
  const effective = own ? nearestLevel(levels, own) || own : fallback;
  const label = own ? `${EFFORT_NAMES[effective] || 'Default'} effort`
              : effective ? `${EFFORT_NAMES[effective]} effort` : 'Default effort';
  const open = state.menu === `effort:${mode}` && !!profile;
  const defaultHint = fallback ? EFFORT_NAMES[fallback] : '';
  const menu = open ? `<div class="popmenu picks" role="menu">
      <div class="mi-caption">EFFORT</div>
      ${menuItem('effort-pick', '', 'Default', defaultHint, !own)}
      ${levels.map((value) => menuItem('effort-pick', value, EFFORT_NAMES[value] || value, '', own === value
          || (!!own && !levels.includes(own) && nearestLevel(levels, own) === value))).join('')}
    </div>` : '';
  return `<div class="menu-wrap">${pickerButton('effort', mode, PICK.effort, profile ? label : 'Effort',
      target ? '' : 'For every expert')}${menu}</div>`;
}

/// Whether it thinks: a switch for a model whose thinking can be switched,
/// held on for one that always thinks, and nothing for one that never does.
function thinkingSwitch() {
  const target = routeTarget();
  const profile = profileOf(target);
  if (!profile || profile.thinking === 'none' || !profile.thinking) return '';
  const on = thinkingOn(target, profile);
  const held = profile.thinking === 'always';
  return `<button type="button" class="picker think${on ? ' on' : ''}" data-act="thinking-toggle"
      aria-pressed="${on}"${held ? ' disabled' : ''} title="${held ? 'Always thinks' : on ? 'Thinking on' : 'Thinking off'}">${
      on ? PICK.think : PICK.unthink}<span class="label">${on ? 'Thinking' : 'No thinking'}</span></button>`;
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
  const value = item.dataset.value || '';
  const target = routeTarget();
  if (!target) {
    // Said at once; the setting is written behind it.
    state.snapshot.reasoning_effort = value;
    configure({ reasoning_effort: value });
    return;
  }
  setSeatReasoning(target, { effort: value });
};

actions['thinking-toggle'] = () => {
  const target = routeTarget();
  const profile = profileOf(target);
  if (!profile || profile.thinking !== 'optional') return;
  const next = thinkingOn(target, profile) ? 'off' : 'on';
  if (!target) {
    // The box's: back to each model's own way rather than "on", which
    // would switch on the ones that do not think unless asked.
    const value = next === 'off' ? 'off' : '';
    state.snapshot.thinking = value;
    configure({ thinking: value });
    return;
  }
  setSeatReasoning(target, { thinking: next });
};

/// Set what one expert has of its own, said at once on screen and written
/// behind it.
function setSeatReasoning(id, change) {
  const seat = ((state.config && state.config.experts) || []).find((e) => e.id === id);
  if (seat) Object.assign(seat, change);
  render();
  guard(async () => {
    await call('expert.reasoning', Object.assign({ id }, change));
    state.config = await call('config');
  });
}

/// The box everything is typed into.
///
/// One box, because there is one way a message is worked: whatever it asks,
/// the expert it is routed to works on it with the tools until it is done,
/// and the delegator brings in another when the next step needs one.
function composerView() {
  const s = state.snapshot;
  const auto_ = !!s.auto_edits;

  // Auto sits beside the box rather than in Settings because whether you are
  // watching an expert edit your files is a decision that changes between one
  // prompt and the next, and a switch you have to go and find is a switch that
  // stays wherever it was last left. It takes effect from the next write, so
  // it is here while a prompt works.
  const autoButton = `<button type="button" class="action toggle" data-act="auto-edits"
      aria-pressed="${auto_}">${auto_ ? 'Auto on' : 'Auto off'}</button>`;

  const buttons = `${autoButton}
      ${s.busy
        ? `<button type="button" class="send stop" data-act="stop" aria-label="${
             s.status === 'stopping' ? 'Stopping' : 'Stop'}">${PICK.stop}</button>`
        : `<button class="send" aria-label="Send">${PICK.up}</button>`}`;

  // One rounded box: what is attached, then the text, then a row along the
  // bottom with the plus at one end and the buttons at the other. More can
  // be typed with tiles in it; they go with whatever is.
  return `<div class="foot">
      <div class="splitter-y" id="composer-splitter" title="Drag to resize"></div>
      <form class="composer" data-submit="send" data-mode="chat">
        <div class="box">
          ${attachTiles('chat')}
          <textarea id="prompt" data-draft data-sized data-key="composer-key" data-input="composer-grow"
                    data-paste="composer-paste"
                    rows="${state.composerRows}" placeholder="Ask anything" autocomplete="off"
                    spellcheck="false">${escape(state.drafts.chat || '')}</textarea>
          <div class="box-bar">
            ${attachButton('chat')}
            ${routePicker('chat')}
            ${effortPicker('chat')}
            ${thinkingSwitch()}
            <span class="spacer"></span>
            ${buttons}
          </div>
        </div>
      </form>
      ${tallyView()}
    </div>`;
}

// --- the view -------------------------------------------------------------------------

/// The conversation: every turn, or somewhere to start when there are none.
///
/// No notices here: the transcript is the conversation and nothing else.
/// What the program has to say goes to the top bar's status line, and a
/// seat that would not load says so on its own row.
function conversationView() {
  const s = state.snapshot;
  const turns = s.turns || [];
  return (turns.length ? turnsView(turns, 0) : readiness()) + (s.pending_edit ? pendingEdit(s.pending_edit) : '');
}

views.chat = () => {
  // The conversation is always here, with the shelf on its right (see
  // panes.js), so what an expert is making can be read while it is being
  // talked about. The same elements in the same order either way: opening
  // the shelf leaves the transcript where it was scrolled and the box as it
  // was typed.
  const open = shelfOpen();
  return sideView() + `<div class="pane chat-pane">
      <div class="split${open ? ' open' : ''}"${open ? ` style="--shelf:${state.shelfShare}%"` : ''}>
        <div class="talk">
          <div class="scroller"><div id="transcript" data-scroll="transcript-scroll">${conversationView()}</div>
            <button class="jump" id="jump" data-act="jump" hidden>${ICONS.down} Jump to latest</button></div>
          ${composerView()}
        </div>${shelfView()}
      </div>
    </div>`;
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
  if (state.snapshot.busy) return;
  // A question can be only its attachments: "here" and a PDF says enough.
  if (!text && !(state.attached.chat || []).length) return;
  const attachments = attachmentsFor('chat');
  if (!attachments) {
    state.error = 'Still reading the attachments';
    return render();
  }
  state.follow = true;
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

/// Where the box's height is found: a copy of it out of the page's flow, as
/// wide as it and in its font, holding what it holds. Found on the box
/// itself, every key typed laid the whole window out twice to learn that the
/// box was the height it already was -- in a long conversation, a tenth of a
/// second a key, and the Enter that sends a prompt waited for it too.
let mirror = null;
let mirrored = null;
let mirrorEdges = 0;
const MIRRORED =['fontFamily', 'fontSize', 'fontWeight', 'fontStyle', 'lineHeight', 'letterSpacing',
                  'paddingTop', 'paddingRight', 'paddingBottom', 'paddingLeft', 'borderTopWidth',
                  'borderRightWidth', 'borderBottomWidth', 'borderLeftWidth', 'borderStyle', 'minHeight',
                  'whiteSpace', 'overflowWrap', 'wordBreak', 'tabSize', 'boxSizing'];

/// Grow the box to fit what is in it, up to a third of the window, and never
/// below the rows the splitter asked for.
function growComposer(box) {
  if (!mirror || !mirror.isConnected) {
    mirror = document.createElement('textarea');
    mirror.setAttribute('aria-hidden', 'true');
    mirror.tabIndex = -1;
    document.body.appendChild(mirror);
    mirrored = null;
  }
  if (mirrored !== box) {
    const style = getComputedStyle(box);
    mirror.style.cssText = 'position:fixed;left:-10000px;top:0;visibility:hidden;height:auto;overflow:hidden';
    for (const name of MIRRORED) mirror.style[name] = style[name];
    mirrorEdges = parseFloat(style.borderLeftWidth) + parseFloat(style.borderRightWidth);
    mirrored = box;
  }
  // As wide as the box less any scroll bar it has, so its lines wrap where
  // the box's do.
  mirror.style.width = `${box.clientWidth + mirrorEdges}px`;
  mirror.rows = box.rows;
  mirror.value = box.value;
  const asked = mirror.offsetHeight;
  const fits = mirror.scrollHeight + mirror.offsetHeight - mirror.clientHeight;
  const height = `${Math.max(asked, Math.min(fits, window.innerHeight / 3))}px`;
  if (box.style.height !== height) box.style.height = height;
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
  // button that does nothing visible gets pressed again.
  state.snapshot.status = 'stopping';
  guard(() => call('cancel'));
};
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

/// Scrolled up to within a screen of the first turn drawn, the turns before
/// it are drawn too, and the one that was first stays where it was on
/// screen (see DRAWN_AT_ONCE).
actions['transcript-scroll'] = (box) => {
  const w = transcriptWindow;
  if (w.from === 0 || w.growing || box.scrollTop > box.clientHeight) return;
  const first = box.firstElementChild;
  w.growing = first ? { key: first.getAttribute('data-key'), top: first.getBoundingClientRect().top } : {};
  w.from = drawnFrom(state.snapshot.turns || [], w.from);
  render();
};
afterDraw.push(() => {
  const grew = transcriptWindow.growing;
  if (!grew) return;
  transcriptWindow.growing = null;
  const box = document.getElementById('transcript');
  const kept = box && grew.key ? Array.from(box.children).find((t) => t.getAttribute('data-key') === grew.key) : null;
  if (kept) box.scrollTop += kept.getBoundingClientRect().top - grew.top;
});

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
  // Each looked up where it is rather than searched for in the whole page,
  // which in a long conversation was every element of every reply.
  const layer = document.getElementById('layer');
  const prompt = document.getElementById('prompt');
  const wanted = (layer && layer.querySelector('[data-focus]'))
      || (state.modal ? null : prompt && !prompt.disabled ? prompt : null);
  if (wanted && wanted !== focused && !(layer && layer.contains(document.activeElement))) {
    // Not out from under somebody typing elsewhere on the page.
    const active = document.activeElement;
    const typing = active && active !== document.body && active !== wanted
        && /^(INPUT|TEXTAREA|SELECT)$/.test(active.tagName);
    if (!typing || wanted.closest('#layer')) wanted.focus();
  }
  focused = wanted || null;
});
