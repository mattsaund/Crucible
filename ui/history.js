// SPDX-License-Identifier: MIT
//
// History: every conversation this project has had.

// With the right-hand panel beside it, as Chat has: History is opened from
// there, and a project picked in it shows that project's.
views.history = () => historyPane() + recentsView();

function historyPane() {
  const project = state.snapshot.project || {};
  if (!project.open) {
    return sideView() + `<div class="pane"><div class="empty">
        <div class="empty-label">No project open.</div></div></div>`;
  }
  const h = state.history;
  if (!h) return sideView() + '<div class="pane"><div class="empty">Reading...</div></div>';

  const sessions = h.sessions.length ? h.sessions.map((c) => `
    <button class="card pick" data-act="history-session" data-id="${escape(c.id)}">
      <div class="title"><strong>${escape(c.title || '(no title)')}</strong></div>
      <div class="hint">${apart(escape(c.when), count(c.turns, 'turn'))}</div>
    </button>`).join('') : '<p class="lede">No conversations yet.</p>';

  return sideView() + `<div class="pane"><div class="settings-page">
      <h1>History</h1>
      <h2>CONVERSATIONS</h2>${sessions}
    </div></div>`;
}

entering.history = () => {
  if ((state.snapshot.project || {}).open) need('history', 'history', true);
};

actions['history-session'] = (e) => guard(async () => {
  await call('history.open', { id: e.dataset.id });
  enter('chat');
});
