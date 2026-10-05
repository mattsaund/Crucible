// SPDX-License-Identifier: MIT
//
// Cook: one goal, worked in passes until it is done or you stop it.
//
// A prompt is one question and one answer; a cook is a goal and however long
// it takes. The view is the journal as it happens, because a cook that only
// reported at the end would be an hour of wondering.

/// The steps of a journal. Shared with Chat, which shows the tail of a cook
/// that is running, and with History, which shows a finished one whole.
///
/// Each step is a verb and what it did. One with something to show for it --
/// the diff of a write, the output of a command -- opens to show it, in a
/// block named for the file it changed when it changed one.
function cookSteps(steps, from) {
  if (!steps || !steps.length) return '<div class="status">Nothing yet.</div>';
  const skipped = from > 0
    ? `<div class="status" style="margin-bottom:1rem">${count(from, 'earlier step')} ${
        from === 1 ? 'is' : 'are'} in the journal</div>` : '';
  return skipped + steps.map((step) => {
    const head = `<span class="kind kind-${escape(step.kind)}${step.ok === false ? ' kind-failed' : ''}">${
        escape(step.kind)}</span>
      <span class="${step.ok === false ? 'bad' : ''}">${escape(step.summary)}</span>
      <span class="tag">${escape(expertName(step.expert))}</span>`;
    if (!step.detail) return `<div class="step"><div class="step-head">${head}</div></div>`;
    const file = (step.changed || [])[0] || '';
    return `<details class="step"><summary class="step-head">${head}</summary>${
      step.kind === 'write' ? writtenBlock(step.detail, file) : codeBlock(step.detail, languageOf(file))}</details>`;
  }).join('');
}

/// Who has had the work, in order: "Programming -> Writing -> Programming".
/// Only said when more than one has, which is when it is worth saying.
function cookChain(cook) {
  const chain = [];
  for (const step of cook.steps || []) {
    if (step.expert && chain[chain.length - 1] !== step.expert) chain.push(step.expert);
  }
  if (chain.length < 2) return '';
  return `<div class="status chain" title="Who has had the work, in the order they had it. A HANDOFF sends the next piece back through the delegator.">${
    chain.map((id) => escape(expertName(id))).join('  →  ')}</div>`;
}

/// One cook, running or finished: the goal, where it is, and what it did.
function cookBody(cook, live) {
  const failed = cook.state === 'failed';
  const running = live && cook.running;
  const dot = failed ? 'missing' : running ? 'active' : 'dormant';
  const files = cook.files || [];
  const done = !running && cook.state !== 'idle';

  // "It says it finished" and "something on disk changed" are different
  // claims, and only one of them can be checked. So the second is checked,
  // and a cook that changed nothing is told so in the one color reserved for
  // things being wrong.
  const changed = !done && !files.length ? ''
    : files.length
      ? `<div class="changed"><span class="status">changed</span> ${
          files.map((f) => `<span class="ok-file">${escape(f)}</span>`).join('  ')}</div>`
      : '<div class="changed bad">changed no files -- whatever it says, nothing on disk moved</div>';

  return `<div class="goal-card">
        <div class="caption">GOAL</div>
        <div class="goal">${escape(cook.goal)}</div>
        ${attachedChips(cook.attachments)}
        ${running ? `<button class="icon goal-stop" data-act="stop"
            title="Stop now, without the finishing pass" aria-label="Stop now">${ICONS.stop}</button>` : ''}
      </div>
      <div class="cook-state seat" data-phase="${dot}">
        <span class="dot"></span><strong>${escape(cook.state)}</strong>
        <span class="status">pass ${cook.iterations || 0}  ·  ${span(cook.seconds)}  ·  ${
          count(cook.total !== undefined ? cook.total : (cook.steps || []).length, 'step')}</span>
      </div>
      ${cookChain(cook)}
      ${cook.state === 'asking' ? `<div class="asking"><div class="caption">IT IS ASKING</div>
          <div>${escape(cook.question)}</div>
          <div class="status" style="margin-top:.5rem">answer in the box below and press enter</div></div>` : ''}
      ${cookSteps(cook.steps, cook.shown_from || 0)}
      ${cook.outcome ? `<hr class="rule"><div class="md outcome">${markdown(cook.outcome)}</div>` : ''}
      ${changed}`;
}

views.cook = () => {
  const cook = state.snapshot.cook;
  // With nothing cooking, the same check Chat makes: a cook needs a project,
  // a runtime and a model exactly as a question does.
  const body = cook ? cookBody(cook, true)
    : readiness(`<div class="empty"><div class="empty-label quiet">Give it a goal</div>
        <div class="status">It works until it is done or until you stop it.</div></div>`);
  // A write waiting for a yes, when Auto is off. Drawn where the journal
  // ends, which is where the cook is.
  const edit = state.snapshot.pending_edit ? pendingEdit(state.snapshot.pending_edit) : '';
  return sideView() + `<div class="pane">
      <div class="scroller"><div id="transcript">${body}${edit}</div>
        <button class="jump" id="jump" data-act="jump" hidden>${ICONS.down} Jump to latest</button></div>
      ${composerView({ cook: true, hint: 'what should it work on?', send: 'Cook' })}
    </div>`;
};

actions['cook-stop'] = () => guard(() => call('cook.stop'));
