// SPDX-License-Identifier: MIT
//
// The conversation: what was asked, who answered and how, and the box the
// next question goes in.

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
    bits.map((b) => `<span class="sep">·</span><span>${escape(b)}</span>`).join('')}</div>`;
}

/// What the turn cost, under it.
function turnMeta(turn) {
  const bits = [];
  if (turn.tokens_per_second > 0) bits.push(`${turn.tokens_per_second.toFixed(1)} tok/s`);
  if (turn.prompt_tokens > 0) bits.push(`${compact(turn.prompt_tokens)} in`);
  if (turn.output_tokens > 0) bits.push(`${compact(turn.output_tokens)} out`);
  if (turn.canceled) bits.push('-- stopped');
  return bits.length ? `<div class="meta">${bits.map(escape).join('  ·  ')}</div>` : '';
}

/// What an expert did on the way to answering: the files it read, the
/// commands it ran, each where it happened. In line rather than folded away,
/// because a write to your project is not a footnote to the sentence that
/// follows it.
function turnActions(turn) {
  return (turn.actions || []).map((a) => `<div class="act"><span class="act-dot">·</span> ${
    escape(a.summary)}${actionBody(a)}</div>`).join('');
}

/// What an action left to look at. A write names the file it wrote, and is
/// drawn as the block that was allowed; anything else is its output.
function actionBody(action) {
  if (!action.body) return '';
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
       <button class="icon" data-act="turn-delete" data-index="${index}" title="Delete this exchange"
               aria-label="Delete this exchange">${ICONS.trash}</button>`;
  return buttons ? `<div class="turn-controls">${buttons}</div>` : '';
}

/// One exchange, as markup.
function turnView(turn, index, busy, showThinking) {
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
  // model look like it has started rambling. Whether it starts open is a
  // setting, and opening or closing one is how the setting is changed.
  const thought = turn.reasoning
    ? `<details class="work thought" data-toggle="thinking"${showThinking ? ' open' : ''}>
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
function turnsView(turns, from) {
  const busy = !!state.snapshot.busy;
  const showThinking = !!state.snapshot.show_reasoning;
  const out = [];
  for (let index = from; index < turns.length; index += 1) {
    const turn = turns[index];
    if (turn.streaming) {
      // Changing with every push, so not worth remembering.
      out.push(`<div class="turn" data-key="t${index}">${
        turnView(turn, index, busy, showThinking)}</div>`);
      continue;
    }
    const signature = `${index}|${busy}|${showThinking}|${(turn.attachments || []).length}|${turn.prompt.length}|${
      turn.reply.length}|${(turn.reasoning || '').length}|${(turn.actions || []).length}|${
      turn.failed}|${turn.canceled}|${turn.output_tokens}`;
    let html = turnCache.get(signature);
    if (html === undefined) {
      html = turnView(turn, index, busy, showThinking);
      if (turnCache.size > 400) turnCache.clear();
      turnCache.set(signature, html);
    }
    out.push(`<div class="turn" data-key="t${index}" data-sig="${escape(signature)}">${html}</div>`);
  }
  return out.join('');
}

// --- nothing to show yet -----------------------------------------------------------

const OPENERS = ['What is in this project?', 'Explain a file to me', 'Find and fix a failing test'];

/// What stands between this window and a first answer, or -- when nothing
/// does -- somewhere to start.
///
/// One thing at a time, in the order they have to be fixed: there is no point
/// saying a model is missing to somebody who has not got a runtime to load
/// it with. Each names the one button that fixes it.
function readiness(ready) {
  const s = state.snapshot;
  const project = s.project || {};
  const seated = (s.experts || []).some((e) => e.phase !== 'unconfigured');
  const anyLocal = (s.experts || []).some((e) => e.phase !== 'unconfigured' && !e.provider);
  const runtimes = state.runtimes;
  const noRuntime = runtimes && runtimes.loadable
      && !runtimes.runtimes.some((r) => r.installed);

  let label = '';
  let fix = '';
  if (!project.open) {
    label = 'No project open';
    fix = '<button class="action" data-act="open-project">Open Project</button>';
  } else if (noRuntime && (anyLocal || !seated)) {
    // Only when something local wants one. A roster answered entirely by
    // providers needs no runtime, and should not be told to install one.
    label = 'No runtime';
    fix = '<button class="action" data-act="settings-page" data-page="runtimes">Settings</button>';
  } else if (!seated) {
    label = 'No model selected';
    fix = '<button class="action" data-act="settings-page" data-page="experts">Settings</button>';
  }
  if (fix) return `<div class="empty"><div class="empty-label">${label}</div>${fix}</div>`;
  // Nothing in the way. `ready` is what the view says then: Chat offers
  // somewhere to start, Cook says what a cook is.
  return ready || `<div class="empty"><div class="empty-label quiet">Ask anything</div>
      <div class="chips">${OPENERS.map((text) =>
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
    s.context_size || 0} tokens. Near the top, the oldest exchanges start to be dropped.`;
  return `<div class="tally${used >= 75 ? ' warm' : ''}" title="${escape(tip)}">${
    compact(usage.input_tokens)} in  ·  ${compact(usage.output_tokens)} out  ·  ${
    used}% context used</div>`;
}

/// The box a prompt, a goal or an answer is typed into.
///
/// One function for Chat and Cook, because it is one box: the same place on
/// the screen, the same Enter, the same reason for being shut. What differs
/// is the hint in it and the word on the button.
function composerView(options) {
  const s = state.snapshot;
  const project = s.project || {};
  const cook = s.cook && s.cook.running ? s.cook : null;
  const asking = cook && cook.state === 'asking';
  const auto_ = !!s.auto_edits;

  let hint = options.hint;
  let disabled = false;
  if (!project.open) { hint = 'open a project to start'; disabled = true; }
  else if (asking) hint = 'answer the question above';
  else if (cook && !options.cook) { hint = 'a cook is running -- it has the experts'; disabled = true; }

  // Auto sits beside the box rather than in Settings because whether you are
  // watching an expert edit your files is a decision that changes between one
  // prompt and the next, and a switch you have to go and find is a switch that
  // stays wherever it was last left. A cook asks too, when it is off, and the
  // switch takes effect from its next write -- so it is here while one runs.
  const autoButton = `<button type="button" class="action toggle" data-act="auto-edits"
      aria-pressed="${auto_}" title="${auto_ ? 'Edits apply as they are made'
                                             : 'Every edit is shown before it lands'}">Auto</button>`;

  let buttons;
  if (cook && options.cook && !asking) {
    // Two stops, because they are different things. The first makes a
    // finishing pass so the project is left in a state that runs; the second
    // is a cancel and leaves it wherever it got to.
    const finishing = cook.state === 'finishing';
    buttons = `${finishing ? `<span class="status composer-note">wrapping up -- finishing
        touches, then it will stop</span>` : ''}
      ${autoButton}
      <button type="button" class="action" data-act="cook-stop" ${finishing ? 'disabled' : ''}
              title="Stop taking new work, and leave the project in a state that runs">Stop and finish</button>
      <button type="button" class="action" data-act="stop"
              title="Stops immediately, without the finishing pass">Stop now</button>`;
    disabled = true;
    hint = finishing ? 'finishing up' : 'cooking';
  } else {
    const send = asking ? 'Answer' : options.send;
    buttons = `${autoButton}
      ${s.busy && !asking
        ? `<button type="button" class="action" data-act="stop">${
             s.status === 'stopping' ? 'Stopping' : 'Stop'}</button>`
        : `<button class="action" ${disabled ? 'disabled' : ''}>${send}</button>`}`;
  }

  // One rounded box: what is attached, then the text, then a row along the
  // bottom with the plus at one end and the buttons at the other. More can
  // be typed with tiles in it; they go with whatever is.
  const mode = options.cook ? 'cook' : 'chat';
  return `<div class="foot">
      <div class="splitter-y" id="composer-splitter" title="Drag to make the box taller"></div>
      <form class="composer" data-submit="send" data-mode="${mode}">
        <div class="box${disabled ? ' shut' : ''}">
          ${disabled ? '' : attachTiles(mode)}
          <textarea id="prompt" data-draft data-key="composer-key" data-input="composer-grow"
                    rows="${state.composerRows}" placeholder="${escape(hint)}" autocomplete="off"
                    spellcheck="false" ${disabled ? 'disabled' : ''}></textarea>
          <div class="box-bar">
            ${attachButton(mode, disabled || asking)}
            <span class="spacer"></span>
            ${buttons}
          </div>
        </div>
      </form>
      ${tallyView()}
    </div>`;
}

// --- the view -------------------------------------------------------------------------

views.chat = () => {
  const s = state.snapshot;
  const turns = s.turns || [];
  const notices = (s.notices || []).length
    ? `<div class="notices">${s.notices.map((n) => `<div>- ${escape(n)}</div>`).join('')}</div>`
    : '';
  // A cook is the same experts doing the same work for longer, so the journal
  // of one that is running is drawn here too rather than only on its own tab:
  // somebody watching Chat should not have to guess why the box is shut.
  const cooking = s.cook && s.cook.running
    ? `<div class="turn cook-here"><div class="who"><span class="seat">◆ cooking</span>
         <span class="sep">·</span><span>${escape(s.cook.goal)}</span></div>
         ${cookSteps(s.cook.steps.slice(-6))}
         <button class="link" data-act="view" data-view="cook">Open the cook</button></div>`
    : '';
  const body = turns.length || cooking ? turnsView(turns, 0) + cooking : readiness();
  return sideView() + `<div class="pane">
      <div class="scroller"><div id="transcript">${notices}${body}${
          s.pending_edit ? pendingEdit(s.pending_edit) : ''}</div>
        <button class="jump" id="jump" data-act="jump" hidden>${ICONS.down} Jump to latest</button></div>
      ${composerView({ hint: 'Ask it something', send: 'Send' })}
    </div>`;
};

// --- what a person can do here ------------------------------------------------------------

actions.send = (form) => {
  const box = form.querySelector('textarea');
  const text = box.value.trim();
  const mode = form.dataset.mode;
  const s = state.snapshot;
  const asking = s.cook && s.cook.running && s.cook.state === 'asking';
  // Text typed while a cook is asking is the answer to it, whichever view it
  // was typed on. The question is on screen; the box under it answers it.
  // What is attached stays where it is for the next prompt.
  if (asking) {
    if (!text) return;
    box.value = '';
    growComposer(box);
    return guard(() => call('cook.answer', { answer: text }));
  }
  if (s.busy) return;
  const files = state.attached[mode] || [];
  // A question can be only its attachments -- "here" and a PDF says enough
  // -- but a goal has to say what it is.
  if (!text && (mode === 'cook' || !files.length)) return;
  const attachments = attachmentsFor(mode);
  if (!attachments) {
    state.error = 'still reading what is attached -- a moment';
    return render();
  }
  box.value = '';
  growComposer(box);
  if (mode === 'cook') {
    return guard(async () => {
      await call('cook.start', { goal: text, attachments });
      state.attached.cook = [];
    });
  }
  state.follow = true;
  return guard(async () => {
    await call('submit', Object.assign(pinned(text), { attachments }));
    state.attached.chat = [];
  });
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

/// Grow the box to fit what is in it, up to a third of the window.
function growComposer(box) {
  box.style.height = 'auto';
  box.style.height = `${Math.min(box.scrollHeight + 2, window.innerHeight / 3)}px`;
}
actions['composer-grow'] = (box) => growComposer(box);

actions.opener = (chip) => {
  const box = document.getElementById('prompt');
  if (!box || box.disabled) return;
  box.value = chip.textContent;
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

/// Opening or closing one "thinking" disclosure is how the preference is set
/// for all of them, so the next reply arrives the way this one was left.
actions.thinking = (details) => {
  if (details.open === !!state.snapshot.show_reasoning) return;
  state.snapshot.show_reasoning = details.open;
  configure({ ui: { show_reasoning: details.open } });
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

actions.jump = () => {
  const transcript = document.getElementById('transcript');
  if (transcript) transcript.scrollTop = transcript.scrollHeight;
};

// --- after each draw -------------------------------------------------------------------------

/// Follow the bottom of the transcript, but only for somebody who is there.
///
/// Scrolling up to read something is a decision, and a reply that is still
/// arriving should not undo it. So the view stays where it was put, and a
/// button offers the way back down instead.
afterDraw.push(({ pinned }) => {
  const transcript = document.getElementById('transcript');
  const jump = document.getElementById('jump');
  if (!transcript) return;
  if (pinned || state.follow) {
    transcript.scrollTop = transcript.scrollHeight;
    state.follow = false;
  }
  if (jump) {
    jump.hidden = transcript.scrollHeight - transcript.scrollTop - transcript.clientHeight < 240;
  }
  if (!transcript.dataset.watching) {
    transcript.dataset.watching = '1';
    transcript.addEventListener('scroll', () => {
      const button = document.getElementById('jump');
      if (button) {
        button.hidden = transcript.scrollHeight - transcript.scrollTop - transcript.clientHeight < 240;
      }
    }, { passive: true });
  }
});

/// Put the caret somewhere useful once, when the thing it belongs in appears:
/// the first field of a dialog, the box under the transcript.
let focused = null;
afterDraw.push(() => {
  const wanted = document.querySelector('#layer [data-focus]')
      || (state.modal ? null : document.querySelector('#prompt:not([disabled])'));
  if (wanted && wanted !== focused && !document.querySelector('#layer :focus')) {
    // Not out from under somebody typing elsewhere on the page.
    const active = document.activeElement;
    const typing = active && active !== document.body && active !== wanted
        && /^(INPUT|TEXTAREA|SELECT)$/.test(active.tagName);
    if (!typing || wanted.closest('#layer')) wanted.focus();
  }
  focused = wanted || null;
});
