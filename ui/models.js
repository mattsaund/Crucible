// SPDX-License-Identifier: MIT
//
// Models: search Hugging Face, choose what to download, and watch it come.
//
// The list on the left is a search of the hub; the right is the repository
// chosen from it -- what it is, what it comes in, whether each will run on
// this machine -- and its README. Downloads are pushed with the snapshot
// (market/downloads.hpp) and drawn wherever they are relevant: a strip above
// the list, and the option they are of.

const MARKET_ICONS = {
  vision: '<svg viewBox="0 0 16 16" width="13" height="13" aria-hidden="true" fill="none" stroke="currentColor" stroke-width="1.3"><path d="M1.5 8s2.4-4.5 6.5-4.5S14.5 8 14.5 8s-2.4 4.5-6.5 4.5S1.5 8 1.5 8z"/><circle cx="8" cy="8" r="2"/></svg>',
  tools: '<svg viewBox="0 0 16 16" width="13" height="13" aria-hidden="true" fill="none" stroke="currentColor" stroke-width="1.3" stroke-linecap="round" stroke-linejoin="round"><path d="M10.5 2.5a3 3 0 0 0-2.9 3.8L2.5 11.4l2.1 2.1 5.1-5.1a3 3 0 0 0 3.8-2.9l-1.9 1.4-1.6-1.6z"/></svg>',
  reasoning: '<svg viewBox="0 0 16 16" width="13" height="13" aria-hidden="true" fill="none" stroke="currentColor" stroke-width="1.3" stroke-linecap="round"><path d="M6 13.5h4M6.5 11.5h3"/><path d="M8 2a4 4 0 0 0-2.4 7.2c.5.4.9 1 .9 1.6V11h3v-.2c0-.6.4-1.2.9-1.6A4 4 0 0 0 8 2z"/></svg>',
  down: '<svg viewBox="0 0 16 16" width="12" height="12" aria-hidden="true" fill="none" stroke="currentColor" stroke-width="1.4" stroke-linecap="round" stroke-linejoin="round"><path d="M8 2.5v8M4.5 7.5 8 11l3.5-3.5M3 13.5h10"/></svg>',
  like: '<svg viewBox="0 0 16 16" width="12" height="12" aria-hidden="true" fill="none" stroke="currentColor" stroke-width="1.3" stroke-linejoin="round"><path d="M8 13.5S2 10 2 6a3 3 0 0 1 6-1 3 3 0 0 1 6 1c0 4-6 7.5-6 7.5z"/></svg>',
  pick: '<svg viewBox="0 0 16 16" width="12" height="12" aria-hidden="true" fill="currentColor"><path d="M8 1.8l1.9 3.9 4.3.6-3.1 3 .7 4.3L8 11.6l-3.8 2 .7-4.3-3.1-3 4.3-.6z"/></svg>',
  close: '<svg viewBox="0 0 16 16" width="12" height="12" aria-hidden="true" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round"><path d="M4 4l8 8M12 4l-8 8"/></svg>',
};

const SORTS = [['best', 'Best match'], ['downloads', 'Most downloads'], ['likes', 'Most likes'],
               ['updated', 'Recently updated']];

const FITS = {
  gpu: ['Fits on GPU', 'fit-good'],
  partial: ['Partial offload', 'fit-some'],
  cpu: ['CPU only', 'fit-some'],
  large: ['Too large', 'fit-bad'],
  mac: ['Mac only', 'fit-bad'],
};

/// "5.8M", "5.3k": a count read at a glance.
function shortCount(n) {
  if (!n) return '0';
  if (n >= 1e6) return `${(n / 1e6).toFixed(n >= 1e7 ? 0 : 1)}M`;
  if (n >= 1e3) return `${(n / 1e3).toFixed(n >= 1e4 ? 0 : 1)}k`;
  return String(n);
}

/// "27B", "600M", "2.4T".
function paramsLabel(b) {
  if (!b || b <= 0) return '';
  if (b < 1) return `${Math.round(b * 1000)}M`;
  if (b >= 1000) return `${Number((b / 1000).toFixed(1))}T`;
  return `${b >= 10 ? Math.round(b) : Number(b.toFixed(1))}B`;
}

/// "today", "3 days ago", "2 months ago". Not base.js's ago, which takes a
/// unix time: every script shares one scope, and this one loads later.
function updatedAgo(iso) {
  const then = Date.parse(iso || '');
  if (!then) return '';
  const days = Math.floor((Date.now() - then) / 86400000);
  if (days < 1) return 'today';
  if (days < 60) return `${count(days, 'day')} ago`;
  if (days < 730) return `${count(Math.floor(days / 30), 'month')} ago`;
  return `${count(Math.floor(days / 365), 'year')} ago`;
}

/// A README as something the page can draw: no front matter, no pictures,
/// no HTML. Links keep their words. Code fences are left as they are.
function tidyReadme(text) {
  let src = String(text || '').replace(/\r\n?/g, '\n');
  src = src.replace(/^\uFEFF?\s*---\n[\s\S]*?\n---[ \t]*(\n|$)/, '');
  src = src.replace(/<!--[\s\S]*?-->/g, '');
  const out = [];
  let fenced = false;
  for (let line of src.split('\n')) {
    if (/^\s*(```|~~~)/.test(line)) { fenced = !fenced; out.push(line); continue; }
    if (fenced) { out.push(line); continue; }
    // Indented by the HTML it sat in, not to say anything; a nested list
    // item keeps its place.
    if (!/^\s*([-*+]|\d+[.)])\s/.test(line)) line = line.trimStart();
    // GitHub's "> [!NOTE]" heads a callout; here it is the word.
    line = line.replace(/^(>\s*)\[!(NOTE|TIP|IMPORTANT|WARNING|CAUTION)\]\s*$/i,
      (_, quote, kind) => `${quote}**${kind[0].toUpperCase()}${kind.slice(1).toLowerCase()}**`);
    line = line.replace(/!\[[^\]]*\]\([^)]*\)/g, '')
      .replace(/<img\b[^>]*>/gi, '')
      .replace(/<br\s*\/?>/gi, ' ')
      .replace(/<\/?[a-zA-Z][^>]*>/g, '')
      .replace(/\[([^\]]+)\]\([^)]*\)/g, '$1')
      .replace(/\[\s*\]\([^)]*\)/g, '')
      .replace(/&nbsp;/g, ' ');
    out.push(line);
  }
  return out.join('\n').replace(/\n{3,}/g, '\n\n').trim().slice(0, 60000);
}

function market() {
  return state.market || (state.market = {
    query: '', format: 'gguf', sort: 'best', results: null, next: '', loading: false, more: false,
    error: '', selected: '', detail: null, readme: null, option: '', asked: 0,
  });
}

/// The downloads, as pushed.
const downloadsNow = () => state.snapshot.downloads || [];

function capabilityIcons(l) {
  return [l.vision ? `<span class="cap" title="Vision">${MARKET_ICONS.vision}</span>` : '',
          l.tools ? `<span class="cap" title="Tool use">${MARKET_ICONS.tools}</span>` : '',
          l.reasoning ? `<span class="cap" title="Reasoning">${MARKET_ICONS.reasoning}</span>` : ''].join('');
}

function resultRow(l) {
  const m = market();
  return `<button class="market-row${m.selected === l.id ? ' here' : ''}" data-act="market-pick" data-id="${escape(l.id)}">
      <div class="market-row-head"><strong>${escape(l.name)}</strong><span class="caps">${capabilityIcons(l)}</span></div>
      <div class="market-row-meta"><span class="who">${apart(escape(l.author), paramsLabel(l.params_b),
        `${MARKET_ICONS.down} ${shortCount(l.downloads)}`, `${MARKET_ICONS.like} ${shortCount(l.likes)}`)}</span>
        <span class="when">${escape(updatedAgo(l.updated))}</span></div>
    </button>`;
}

function downloadBar(d) {
  const share = d.total ? Math.min(1, d.done / d.total) : 0;
  const left = d.rate > 0 && d.total > d.done ? Math.round((d.total - d.done) / d.rate) : 0;
  const words = d.state === 'starting' ? 'Starting'
    : d.state === 'running' ? apart(`${bytes(d.done)} of ${bytes(d.total)}`,
                                    d.rate > 0 ? `${bytes(d.rate)}/s` : '', left ? span(left) : '')
    : d.state === 'done' ? 'Downloaded'
    : d.state === 'canceled' ? 'Canceled'
    : escape(d.error || 'Failed');
  const over = ['done', 'failed', 'canceled'].includes(d.state);
  return `<div class="download${d.state === 'failed' ? ' failed' : ''}">
      <div class="row"><strong>${escape(d.repo.split('/').pop())}</strong><span class="tag">${escape(d.option)}</span>
        <span class="spacer"></span>
        ${over ? `<button class="icon" data-act="market-dismiss" data-id="${escape(d.id)}" title="Clear" aria-label="Clear">${MARKET_ICONS.close}</button>`
               : `<button class="icon" data-act="market-cancel" data-id="${escape(d.id)}" title="Cancel" aria-label="Cancel">${MARKET_ICONS.close}</button>`}</div>
      ${over ? '' : `<span class="bar"><span style="width:${(share * 100).toFixed(1)}%"></span></span>`}
      <div class="hint">${words}</div>
    </div>`;
}

/// What an option is, on one line: its format, its name, the suggestion,
/// whether it is here or on its way, and (in the menu) whether it will run.
function optionLine(o, format, withFit) {
  const going = downloadsNow().some((d) => d.repo === o.repo && d.option === o.name
                                         && (d.state === 'running' || d.state === 'starting'));
  const [fitWords, fitClass] = FITS[o.fit] || FITS.large;
  return `<span class="tag">${format === 'mlx' ? 'MLX' : 'GGUF'}</span>
      <strong>${escape(o.name)}</strong>
      ${o.recommended ? `<span class="pick-mark" title="Suggested">${MARKET_ICONS.pick}</span>` : ''}
      <span class="spacer"></span>
      ${o.here ? '<span class="status">here</span>' : going ? '<span class="status ok">downloading</span>' : ''}
      ${withFit ? `<span class="fit ${fitClass}">${fitWords}</span>` : ''}
      <span class="status size">${bytes(o.bytes)}</span>`;
}

/// The option chosen, which opens into every option, LM Studio's way.
function optionPicker(d, option) {
  const open = state.menu === 'market-option';
  const menu = open ? `<div class="popmenu below market-choices" role="menu">
      ${d.options.map((o) => `<button type="button" role="menuitemradio" aria-checked="${o.name === option.name}"
          data-act="market-option" data-name="${escape(o.name)}">${optionLine(o, d.format, true)}<span class="mi-check">${
          o.name === option.name ? PICK.check : ''}</span></button>`).join('')}
    </div>` : '';
  return `<div class="menu-wrap market-choice-wrap"><button type="button" class="market-choice"
      data-act="market-option-menu" aria-haspopup="menu" aria-expanded="${open}">${
      optionLine(option, d.format, false)}${PICK.chevron}</button>${menu}</div>`;
}

function detailView() {
  const m = market();
  if (!m.selected) return '<div class="empty"><div class="empty-label quiet">Choose a model.</div></div>';
  const d = m.detail;
  if (!d || d.id !== m.selected) return m.error ? `<div class="bad">${escape(m.error)}</div>` : '<div class="status">Reading...</div>';
  const option = d.options.find((o) => o.name === m.option) || d.options.find((o) => o.recommended) || d.options[0];
  const going = option && downloadsNow().find((x) => x.repo === option.repo && x.option === option.name
                                                     && (x.state === 'running' || x.state === 'starting'));
  const facts = [['PARAMS', paramsLabel(d.params_b)], ['ARCH', d.architecture],
                 ['CONTEXT', d.context ? `${Math.round(d.context / 1000)}K` : ''],
                 ['FORMAT', d.format === 'mlx' ? 'MLX' : 'GGUF'], ['LICENSE', d.license]]
    .filter(([, v]) => v)
    .map(([k, v]) => `<span class="fact"><span class="caption">${k}</span> ${escape(v)}</span>`).join('');
  const caps = [d.vision ? 'Vision' : '', d.tools ? 'Tool use' : '', d.reasoning ? 'Reasoning' : '']
    .filter(Boolean).map((c) => `<span class="cap-chip">${c}</span>`).join('');
  let action = '';
  if (d.gated) {
    action = '<button class="action" disabled title="Accept its license on Hugging Face first">Gated</button>';
  } else if (option && option.here) {
    action = '<button class="action" disabled>Downloaded</button>';
  } else if (going) {
    action = `<button class="action" data-act="market-cancel" data-id="${escape(going.id)}">Cancel</button>`;
  } else if (option) {
    action = `<button class="action primary" data-act="market-download">Download ${bytes(option.bytes)}</button>`;
  }
  const [fitWords, fitClass] = option ? (FITS[option.fit] || FITS.large) : ['', ''];
  const readme = m.readme === null ? '<div class="status">Reading...</div>'
    : m.readme ? `<div class="md readme">${markdown(m.readme)}</div>` : '<div class="status">No README.</div>';
  return `<div class="market-head">
      <h1>${escape(d.id)}</h1>
      <div class="status">${apart(`${MARKET_ICONS.down} ${shortCount(d.downloads)}`,
        `${MARKET_ICONS.like} ${shortCount(d.likes)}`, d.updated ? `updated ${escape(updatedAgo(d.updated))}` : '',
        d.base_model ? `from ${escape(d.base_model)}` : '')}</div>
    </div>
    <div class="facts">${facts}</div>
    ${caps ? `<div class="facts">${caps}</div>` : ''}
    <h2>DOWNLOAD</h2>
    ${d.options.length ? `${optionPicker(d, option)}
      <div class="row market-go">${option ? `<span class="fit ${fitClass}">${fitWords}</span>` : ''}
        <span class="spacer"></span>${action}</div>`
      : '<p class="lede">Nothing to download here.</p>'}
    ${going ? downloadBar(going) : ''}
    <h2>README</h2>
    ${readme}`;
}

/// How the list is ordered: a menu like the box's, hanging down.
function sortPicker() {
  const m = market();
  const open = state.menu === 'market-sort';
  const label = (SORTS.find(([v]) => v === m.sort) || SORTS[0])[1];
  const menu = open ? `<div class="popmenu picks below" role="menu">
      <div class="mi-caption">SORT BY</div>
      ${SORTS.map(([value, words]) => menuItem('market-sort', value, words, '', value === m.sort)).join('')}
    </div>` : '';
  return `<div class="menu-wrap"><button type="button" class="picker" data-act="market-sort-menu" aria-haspopup="menu"
      aria-expanded="${open}"><span class="label">${escape(label)}</span>${PICK.chevron}</button>${menu}</div>`;
}

views.models = () => {
  const m = market();
  const results = m.results === null
    ? `<div class="status">${m.loading ? 'Searching...' : ''}</div>`
    : m.results.length ? m.results.map(resultRow).join('')
                       + (m.next ? `<button class="action wide" data-act="market-more" ${m.more ? 'disabled' : ''}>${
                           m.more ? 'Reading...' : 'More'}</button>` : '')
    : '<div class="status">Nothing found.</div>';
  const downloads = downloadsNow();
  return sideView() + `<div class="pane"><div class="market">
      <div class="market-list">
        <input id="market-query" class="market-search" data-draft data-input="market-type" data-key="market-key"
               placeholder="Search Hugging Face" spellcheck="false" autocomplete="off" value="${escape(m.query)}">
        <div class="row market-controls">
          <div class="segmented">
            <button class="action toggle" data-act="market-format" data-format="gguf" aria-pressed="${m.format === 'gguf'}">GGUF</button>
            <button class="action toggle" data-act="market-format" data-format="mlx" aria-pressed="${m.format === 'mlx'}">MLX</button>
          </div>
          <span class="spacer"></span>
          ${sortPicker()}
        </div>
        ${downloads.length ? `<div class="downloads">${downloads.map(downloadBar).join('')}</div>` : ''}
        ${m.error && !m.selected ? `<div class="bad">${escape(m.error)}</div>` : ''}
        <div class="market-results">${results}</div>
      </div>
      <div class="market-detail">${detailView()}</div>
    </div></div>`;
};

/// Search from the top, and draw what comes back unless a newer one went out.
function marketSearch() {
  const m = market();
  const asked = ++m.asked;
  m.loading = true;
  m.error = '';
  render();
  call('market.search', { query: m.query.trim(), format: m.format, sort: m.sort })
    .then((page) => { if (asked === m.asked) { m.results = page.listings; m.next = page.next; } })
    .catch((error) => { if (asked === m.asked) { m.error = error.message; m.results = []; } })
    .finally(() => { if (asked === m.asked) { m.loading = false; render(); } });
}

let marketTyping = null;
actions['market-type'] = (box) => {
  market().query = box.value;
  clearTimeout(marketTyping);
  marketTyping = setTimeout(marketSearch, 350);
};
actions['market-key'] = (box, event) => {
  if (event.key === 'Enter') { clearTimeout(marketTyping); market().query = box.value; marketSearch(); }
};
actions['market-format'] = (button) => {
  const m = market();
  if (m.format === button.dataset.format) return;
  m.format = button.dataset.format;
  m.selected = '';
  m.detail = null;
  marketSearch();
};
actions['market-sort-menu'] = () => {
  state.menu = state.menu === 'market-sort' ? null : 'market-sort';
  render();
};
actions['market-sort'] = (item) => {
  state.menu = null;
  if (market().sort === item.dataset.value) { render(); return; }
  market().sort = item.dataset.value;
  marketSearch();
};
actions['market-more'] = () => {
  const m = market();
  if (!m.next || m.more) return;
  const asked = m.asked;
  m.more = true;
  render();
  call('market.search', { next: m.next, sort: m.sort })
    .then((page) => { if (asked === m.asked) { m.results = m.results.concat(page.listings); m.next = page.next; } })
    .catch((error) => { m.error = error.message; })
    .finally(() => { m.more = false; render(); });
};

/// A repository, and its README beside it.
function marketOpen(id) {
  const m = market();
  m.selected = id;
  m.detail = null;
  m.readme = null;
  m.option = '';
  m.error = '';
  render();
  call('market.model', { id, format: m.format })
    .then((detail) => {
      if (m.selected !== id) return;
      m.detail = detail;
      // The suggestion, or else the one that was clicked: an MLX
      // repository's siblings are its other quantizations.
      const suggested = detail.options.find((o) => o.recommended) || detail.options.find((o) => o.repo === id);
      m.option = suggested ? suggested.name : (detail.options[0] || {}).name || '';
    })
    .catch((error) => { if (m.selected === id) m.error = error.message; })
    .finally(render);
  call('market.readme', { id })
    .then((reply) => { if (m.selected === id) m.readme = tidyReadme(reply.text); })
    .catch(() => { if (m.selected === id) m.readme = ''; })
    .finally(render);
}
actions['market-pick'] = (row) => marketOpen(row.dataset.id);
/// The menu opens at the option chosen, not at the smallest.
let choicesOpen = false;
afterDraw.push(() => {
  const open = state.menu === 'market-option';
  if (open && !choicesOpen) {
    const menu = document.querySelector('.market-choices');
    const chosen = menu && menu.querySelector('[aria-checked="true"]');
    if (chosen) menu.scrollTop = chosen.offsetTop - (menu.clientHeight - chosen.offsetHeight) / 2;
  }
  choicesOpen = open;
});
actions['market-option-menu'] = () => {
  state.menu = state.menu === 'market-option' ? null : 'market-option';
  render();
};
actions['market-option'] = (item) => {
  market().option = item.dataset.name;
  state.menu = null;
  render();
};
actions['market-download'] = () => guard(async () => {
  const m = market();
  const option = (m.detail.options || []).find((o) => o.name === m.option);
  if (!option) return;
  await call('market.download', { repo: option.repo, option: option.name, format: m.detail.format });
});
actions['market-cancel'] = (button) => guard(() => call('market.cancel', { id: button.dataset.id }));
actions['market-dismiss'] = (button) => guard(() => call('market.dismiss', { id: button.dataset.id }));

entering.models = () => {
  if (market().results === null) marketSearch();
};

/// A download that has just finished is a model the rest of the program can
/// now choose: the models list is read again, and the option says so.
let downloadsDone = '';
onSnapshot.push(() => {
  const done = downloadsNow().filter((d) => d.state === 'done').map((d) => d.id).join(',');
  if (done === downloadsDone) return;
  downloadsDone = done;
  if (!done) return;
  need('models', 'models', true);
  const m = market();
  if (m.detail && m.selected) {
    // Said at once; the hub is asked again behind it.
    for (const o of m.detail.options) {
      if (downloadsNow().some((d) => d.state === 'done' && d.repo === o.repo && d.option === o.name)) o.here = true;
    }
    call('market.model', { id: m.selected, format: m.format })
      .then((detail) => { if (m.selected === detail.id) m.detail = detail; })
      .catch(() => {})
      .finally(render);
  }
});
