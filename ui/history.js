// SPDX-License-Identifier: MIT
//
// History: every cook and every conversation this project has had.

views.history = () => {
  const project = state.snapshot.project || {};
  if (!project.open) {
    return sideView() + `<div class="pane"><div class="empty">
        <div class="empty-label">No project open.</div></div></div>`;
  }
  if (state.open.cook) {
    return sideView() + `<div class="pane"><div id="transcript">
        <button class="action" data-act="history-back" style="margin-bottom:1.2rem">All history</button>
        ${cookBody(state.open.cook, false)}
      </div></div>`;
  }
  const h = state.history;
  if (!h) return sideView() + '<div class="pane"><div class="empty">Reading...</div></div>';

  const cooks = h.cooks.length ? h.cooks.map((c) => `
    <button class="card pick" data-act="history-cook" data-id="${escape(c.id)}">
      <div class="title"><strong>${escape(c.goal)}</strong></div>
      <div class="hint">${escape(c.when)}  ·  ${count(c.files, 'file')}  ·  ${
        count(c.steps, 'step')}  ·  ${span(c.seconds)}  ·  ${escape(c.state)}</div>
    </button>`).join('') : '<p class="lede">No cooks yet.</p>';

  const sessions = h.sessions.length ? h.sessions.map((c) => `
    <button class="card pick" data-act="history-session" data-id="${escape(c.id)}">
      <div class="title"><strong>${escape(c.title || '(no title)')}</strong></div>
      <div class="hint">${escape(c.when)}  ·  ${count(c.turns, 'turn')}</div>
    </button>`).join('') : '<p class="lede">No conversations yet.</p>';

  return sideView() + `<div class="pane"><div class="settings-page">
      <h1>History</h1>
      <h2>COOKS</h2>${cooks}
      <h2 style="margin-top:1.8rem">CONVERSATIONS</h2>${sessions}
    </div></div>`;
};

entering.history = () => {
  state.open.cook = null;
  if ((state.snapshot.project || {}).open) need('history', 'history', true);
};

actions['history-back'] = () => { state.open.cook = null; render(); };
actions['history-cook'] = (e) => guard(async () => {
  state.open.cook = await call('history.cook', { id: e.dataset.id });
});
actions['history-session'] = (e) => guard(async () => {
  await call('history.open', { id: e.dataset.id });
  enter('chat');
});
