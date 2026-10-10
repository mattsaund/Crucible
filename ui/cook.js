// SPDX-License-Identifier: MIT
//
// A journal, drawn: the steps a build or a cook took, and a whole one as
// History shows it.
//
// There was a Cook tab beside Build -- one goal worked in passes until it
// was stopped -- and it is folded into Build, which does the same work with
// a plan on top. The journal it kept is the journal a build keeps, so what
// draws one draws the other, and a cook from before opens from History as it
// always did. Chat shows the tail of whichever is running.

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
      <span class="${step.ok === false ? 'bad' : ''}">${escape(step.summary)}${pictureChip(step.picture)}</span>
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

/// A past build's agent, chosen where it is drawn.
actions['task-open'] = (button) => { state.open.task = Number(button.dataset.index); render(); };

/// A cook's state as it stands. A journal left in the middle of one --
/// Crucible closed, or the build was ended, while it worked -- was
/// interrupted, and says so rather than "asking" for ever after.
function settled(cook) {
  const now = state.snapshot.cook;
  const running = !!(now && now.running && now.id === cook.id);
  return !running && ['working', 'asking', 'finishing'].includes(cook.state) ? 'interrupted' : cook.state;
}

/// One cook, running or finished: the goal, where it is, and what it did.
function cookBody(cook, live) {
  // A build, opened from History: the plan and its agents, as the Build
  // view draws them, over the same journal.
  if (cook.kind === 'build' && cook.tasks) {
    const chosen = openTask(cook);
    return `<div class="goal-card"><div class="caption">DIRECTIVE</div><div class="goal">${escape(cook.goal)}</div>
        ${attachedChips(cook.attachments)}</div>
      <div class="cook-state seat" data-phase="${cook.state === 'failed' ? 'missing' : 'dormant'}">
        <span class="dot"></span><strong>${escape(settled(cook))}</strong>
        <span class="status">${apart(count((cook.tasks || []).length, 'task'), span(cook.seconds),
          count(cook.total !== undefined ? cook.total : (cook.steps || []).length, 'step'))}</span></div>
      <div class="build-split">${agentsList(cook, chosen, 'task-open')}${agentWork(cook, chosen)}</div>
      ${cook.outcome ? `<hr class="rule"><div class="md outcome">${markdown(cook.outcome)}</div>` : ''}`;
  }
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
      : '<div class="changed bad">no files changed</div>';

  return `<div class="goal-card">
        <div class="caption">GOAL</div>
        <div class="goal">${escape(cook.goal)}</div>
        ${attachedChips(cook.attachments)}
        ${running ? `<button class="icon goal-stop" data-act="build-cancel"
            title="Stop now" aria-label="Stop now">${ICONS.stop}</button>` : ''}
      </div>
      <div class="cook-state seat" data-phase="${dot}">
        <span class="dot"></span><strong>${escape(settled(cook))}</strong>
        <span class="status">${apart(`pass ${cook.iterations || 0}`, span(cook.seconds),
          count(cook.total !== undefined ? cook.total : (cook.steps || []).length, 'step'))}</span>
      </div>
      ${cookChain(cook)}
      ${settled(cook) === 'asking' ? `<div class="asking"><div class="caption">IT IS ASKING</div>
          <div>${escape(cook.question)}</div>
          <div class="status" style="margin-top:.5rem">answer below</div></div>` : ''}
      ${cookSteps(cook.steps, cook.shown_from || 0)}
      ${cook.outcome ? `<hr class="rule"><div class="md outcome">${markdown(cook.outcome)}</div>` : ''}
      ${changed}`;
}

actions['cook-stop'] = () => guard(() => call('cook.stop'));
