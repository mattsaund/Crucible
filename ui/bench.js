// SPDX-License-Identifier: MIT
//
// Bench: how good a model is, measured the same way everywhere.
//
// Nine benchmarks in seven skills, asked of each model on this machine with
// greedy decoding and checked by the orchestrator (bench.py). The results are
// kept here, drawn as a spider chart against each other, and can be posted to
// the public board, where everybody's results sit side by side: which local
// models are worth the download, by numbers anyone can rerun.
//
// The board is a GitHub repository (mattsaund/crucible-bench). It takes names
// of a fixed form and no free text, and a model only when its file is public
// on Hugging Face. What it holds was posted by other people all the same, so
// it is drawn as text and never as markup.

const BENCH_SUITE = [
  ['mmlu_pro',   'MMLU-Pro',      'knowledge'],
  ['arc',        'ARC-Challenge', 'reasoning'],
  ['gsm8k',      'GSM8K',         'math'],
  ['math500',    'MATH-500',      'math'],
  ['humaneval',  'HumanEval',     'code'],
  ['mbpp',       'MBPP',          'code'],
  ['ifeval',     'IFEval',        'instructions'],
  ['hellaswag',  'HellaSwag',     'commonsense'],
  ['truthfulqa', 'TruthfulQA',    'truthfulness'],
];
const BENCH_SKILLS = [
  ['knowledge', 'Knowledge'], ['reasoning', 'Reasoning'], ['math', 'Math'], ['code', 'Code'],
  ['instructions', 'Instructions'], ['commonsense', 'Commonsense'], ['truthfulness', 'Truthfulness'],
];
const BENCH_SIZES = [['quick', 'Quick', 50], ['standard', 'Standard', 200], ['full', 'Full', 0]];
// How hard a thinking model thinks. Default is the model's own; a result
// says which it ran at, and so does the board.
const BENCH_EFFORTS = [['', 'Default'], ['low', 'Low'], ['medium', 'Medium'], ['high', 'High']];
const BENCH_TOTALS = { mmlu_pro: 12032, arc: 1172, gsm8k: 1319, math500: 500, humaneval: 164, mbpp: 257,
                       ifeval: 541, hellaswag: 10042, truthfulqa: 817 };

// The spider chart's three series colors: blue, magenta and green, validated
// as a set on the page's own black for a chart where every pair can cross
// (13.0 apart at worst for color-blind eyes). None is orange, which on this
// screen means "look here". A model keeps its color while it is compared,
// and the chart takes three.
const BENCH_COLORS = ['#3987e5', '#d55181', '#008300'];

state.bench = {
  picked: {},                   // model path -> chosen to run
  size: 'quick',
  effort: '',
  off: {},                      // benchmark id -> left out of the next run
  results: null,                // kept here, newest first, without their items
  board: null,                  // the public board
  boardQuery: '',
  compare: [],                  // [{ key, color, entry }]: what the chart shows
  open: null,                   // a result opened in full
};

// --- the arithmetic, the board's own ---------------------------------------------------

/// Skills from benchmark counts: each the mean of its benchmarks' scores,
/// from the counts, unrounded. The overall is the mean of the skills there are.
function benchSkills(counts) {
  const out = {};
  for (const [skill] of BENCH_SKILLS) {
    const scores = BENCH_SUITE.filter(([id, , s]) => s === skill && counts && counts[id] && counts[id].n)
      .map(([id]) => counts[id].correct / counts[id].n);
    if (scores.length) out[skill] = scores.reduce((a, b) => a + b, 0) / scores.length;
  }
  return out;
}
function benchOverall(skills) {
  const values = Object.values(skills || {});
  return values.length ? values.reduce((a, b) => a + b, 0) / values.length : null;
}
const pct = (x) => (x === null || x === undefined ? '' : `${(x * 100).toFixed(1)}`);

/// How many items a run asks, for the size chosen.
function benchItems(size, off) {
  const per = (BENCH_SIZES.find(([id]) => id === size) || BENCH_SIZES[0])[2];
  return BENCH_SUITE.filter(([id]) => !off[id])
    .reduce((sum, [id]) => sum + (per ? Math.min(per, BENCH_TOTALS[id]) : BENCH_TOTALS[id]), 0);
}

/// How many answers of a result ran out of room before they were done.
const benchCut = (r) => Object.values(r.cut_off || {}).reduce((a, n) => a + (Number(n) || 0), 0);

/// A benchmark's name, from its id.
const benchTitle = (id) => (BENCH_SUITE.find(([x]) => x === id) || [id, id])[1];

/// A result's name as people say it: its file, the way the Models list and
/// the run panel show it, without the extension. The name inside a GGUF is
/// often a converter's ("Openai_Gpt Oss 20b"), and goes on the hover text of a
/// result kept here; the board has none, and its hover text is the repository.
function benchName(r) {
  const model = r.model || {};
  const file = String(model.file || '').replace(/\.gguf$/i, '');
  return file || model.name || r.name || 'a model';
}

// --- the spider chart -------------------------------------------------------------------

/// The chart, drawn from `series`: [{ label, color, skills }], skills in 0..1.
/// Hairline rings at a quarter, half, three quarters and all; a 2px outline
/// and a tenth-opacity wash per model; each point a ringed dot whose value
/// shows on hover and on keyboard focus.
function radarSvg(series) {
  const size = 420;
  const c = size / 2;
  const r = 132;
  const n = BENCH_SKILLS.length;
  const at = (i, v) => {
    const angle = -Math.PI / 2 + (i * 2 * Math.PI) / n;
    return [c + Math.cos(angle) * r * v, c + Math.sin(angle) * r * v];
  };
  const ring = (v) => BENCH_SKILLS.map((_, i) => at(i, v).map((x) => x.toFixed(1)).join(',')).join(' ');
  const grid = [0.25, 0.5, 0.75, 1].map((v) => `<polygon class="radar-ring" points="${ring(v)}"/>`).join('')
    + BENCH_SKILLS.map((_, i) => { const [x, y] = at(i, 1); return `<line class="radar-ring" x1="${c}" y1="${c}" x2="${x.toFixed(1)}" y2="${y.toFixed(1)}"/>`; }).join('');
  const ticks = [0.5, 1].map((v) => { const [x, y] = at(0, v); return `<text class="radar-tick" x="${(x + 4).toFixed(1)}" y="${(y + 12).toFixed(1)}">${v * 100}%</text>`; }).join('');
  const labels = BENCH_SKILLS.map(([, name], i) => {
    const [x, y] = at(i, 1.17);
    const anchor = Math.abs(x - c) < 4 ? 'middle' : x > c ? 'start' : 'end';
    return `<text class="radar-axis" x="${x.toFixed(1)}" y="${(y + 4).toFixed(1)}" text-anchor="${anchor}">${name}</text>`;
  }).join('');
  // Every outline first and every point after, so no model's wash or line
  // runs across another's readout.
  const pointsOf = (s) => BENCH_SKILLS.map(([skill], i) => (s.skills[skill] === undefined ? null : [i, skill, at(i, s.skills[skill])]))
    .filter(Boolean);
  const shapes = series.map((s) => {
    const points = pointsOf(s);
    if (!points.length) return '';
    const path = points.map(([, , [x, y]]) => `${x.toFixed(1)},${y.toFixed(1)}`).join(' ');
    return `<polygon class="radar-shape" points="${path}" stroke="${s.color}" fill="${s.color}"/>`;
  }).join('');
  const marks = series.map((s) => {
    const dots = pointsOf(s).map(([i, skill, [x, y]]) => {
      const name = BENCH_SKILLS[i][1];
      const value = `${pct(s.skills[skill])}%`;
      const tipX = Math.min(size - 70, Math.max(70, x));
      const tipY = y < c ? y - 14 : y + 24;
      return `<g class="radar-point" tabindex="0" aria-label="${escape(s.label)}, ${name}: ${value}">
          <circle class="radar-hit" cx="${x.toFixed(1)}" cy="${y.toFixed(1)}" r="12"/>
          <circle class="radar-dot" cx="${x.toFixed(1)}" cy="${y.toFixed(1)}" r="4" fill="${s.color}"/>
          <g class="radar-tip"><rect x="${(tipX - 64).toFixed(1)}" y="${(tipY - 15).toFixed(1)}" width="128" height="20" rx="4"/>
            <text x="${tipX.toFixed(1)}" y="${(tipY - 1).toFixed(1)}" text-anchor="middle"><tspan class="radar-tip-value">${value}</tspan> ${escape(name)}</text></g>
        </g>`;
    }).join('');
    return `<g class="radar-series">${dots}</g>`;
  }).join('');
  return `<svg class="radar" viewBox="0 0 ${size} ${size}" role="img" aria-label="Skills, compared">
      ${grid}${ticks}${labels}${shapes}${marks}</svg>`;
}

/// What is on the chart, as rows of numbers: the chart's table twin, and the
/// comparison the chart cannot make past three models.
function compareTable(rows, withColor) {
  if (!rows.length) return '';
  const best = {};
  for (const [skill] of BENCH_SKILLS) best[skill] = Math.max(...rows.map((x) => (x.skills[skill] === undefined ? -1 : x.skills[skill])));
  best.overall = Math.max(...rows.map((x) => (x.overall === null ? -1 : x.overall)));
  // The best of each column stands out, once there is something to beat.
  const cell = (v, b) => `<td class="${rows.length > 1 && v !== undefined && v !== null && v === b ? 'best' : ''}">${
    v === undefined || v === null ? '' : pct(v)}</td>`;
  return `<table class="bench-table"><thead><tr><th>Model</th><th>Overall</th>${
      BENCH_SKILLS.map(([, name]) => `<th>${name}</th>`).join('')}<th>Tok/s</th><th>Size</th></tr></thead><tbody>${
      rows.map((x) => `<tr>
        <td class="model-cell">${withColor && x.color ? `<span class="key-line" style="background:${x.color}"></span>` : ''}<span title="${escape(x.title || x.label)}">${escape(x.label)}</span>${
          x.note ? ` <span class="status">${escape(x.note)}</span>` : ''}</td>
        ${cell(x.overall, best.overall)}
        ${BENCH_SKILLS.map(([skill]) => cell(x.skills[skill], best[skill])).join('')}
        <td>${x.speed ? escape(String(x.speed)) : ''}</td><td>${escape(x.size || '')}</td></tr>`).join('')}</tbody></table>`;
}

/// One comparable row from a result, local or from the board.
function rowOf(entry, extra) {
  const skills = entry.skills && Object.keys(entry.skills).length ? entry.skills : benchSkills(entry.benchmarks);
  const overall = entry.overall !== undefined && entry.overall !== null ? entry.overall : benchOverall(skills);
  const quant = entry.model && entry.model.quant ? entry.model.quant : '';
  const name = benchName(entry);
  return Object.assign({
    // The quantization, unless the file's name says it already.
    label: name + (quant && !name.toLowerCase().includes(quant.toLowerCase()) ? ` ${quant}` : ''),
    title: entry.model ? [entry.model.name, entry.model.repo, entry.model.file].filter(Boolean).join(', ') : '',
    skills, overall,
    // An overall of fewer than every skill is not the same measure, and says so.
    partial: Object.keys(skills).length < BENCH_SKILLS.length,
    speed: entry.speed && entry.speed.gen_tps ? entry.speed.gen_tps : '',
    size: entry.size || '',
  }, extra || {});
}

// --- the view -----------------------------------------------------------------------------

function benchRunView() {
  const b = state.bench;
  const live = state.snapshot.bench;
  const running = !!(live && live.running);
  const models = (state.models ? state.models.models || [] : [])
    .filter((m) => (m.format || 'gguf') === 'gguf' && !/mmproj/i.test(m.name));
  const chosen = models.filter((m) => b.picked[m.path]);
  const items = benchItems(b.size, b.off) * Math.max(1, chosen.length);
  return `<div class="bench-run">
      <div class="caption">MODELS</div>
      <div class="chips left bench-models">${models.length ? models.map((m) => `
        <button class="chip${b.picked[m.path] ? ' on' : ''}" data-act="bench-model" data-path="${escape(m.path)}"
                aria-pressed="${!!b.picked[m.path]}" title="${escape(m.name)}">${escape(fileName(m.name))}
          <span class="status">${bytes(m.bytes)}</span></button>`).join('')
        : '<span class="status">No models yet.</span>'}</div>
      <div class="caption" style="margin-top:.9rem">SIZE</div>
      <div class="chips left">${BENCH_SIZES.map(([id, name, n]) => `
        <button class="chip${b.size === id ? ' on' : ''}" data-act="bench-size" data-size="${id}" aria-pressed="${b.size === id}"
                title="${n ? `${n} items per benchmark` : 'Every item'}">${name}</button>`).join('')}</div>
      <div class="caption" style="margin-top:.9rem">EFFORT</div>
      <div class="chips left">${BENCH_EFFORTS.map(([id, name]) => `
        <button class="chip${b.effort === id ? ' on' : ''}" data-act="bench-effort" data-effort="${id}"
                aria-pressed="${b.effort === id}">${name}</button>`).join('')}</div>
      <div class="caption" style="margin-top:.9rem">BENCHMARKS</div>
      <div class="chips left">${BENCH_SUITE.map(([id, name, skill]) => `
        <button class="chip${b.off[id] ? '' : ' on'}" data-act="bench-toggle" data-id="${id}" aria-pressed="${!b.off[id]}"
                title="${escape(skill)}">${name}</button>`).join('')}</div>
      <div class="row" style="margin-top:1rem">
        ${running ? '<button class="action" data-act="bench-stop">Stop</button>'
                  : `<button class="action primary" data-act="bench-start" ${chosen.length ? '' : 'disabled'}>Run</button>`}
        <span class="status">${apart(chosen.length ? `${items} questions` : 'Pick a model',
          !b.off.humaneval || !b.off.mbpp ? 'Code tests run here' : '')}</span></div>
    </div>`;
}

/// The running clock in that view moves on its own. A run fetching its pages,
/// or a model thinking through one long question, pushes nothing for minutes,
/// and the time stood at "18s" all the while.
function benchTick() {
  const live = state.snapshot && state.snapshot.bench;
  if (live && live.running && state.view === 'bench') render();
  setTimeout(benchTick, 1000);
}
setTimeout(benchTick, 1000);

/// How a run is going: a row per model, a bar per benchmark.
function benchLiveView() {
  const live = state.snapshot.bench;
  if (!live || !live.models || !live.models.length) return '';
  const doneOf = (m) => Object.values(m.benchmarks || {}).reduce((a, x) => a + (x.done || 0), 0);
  const done = live.models.reduce((sum, m) => sum + doneOf(m), 0);
  const each = live.benchmarks.reduce((sum, id) => {
    const per = (BENCH_SIZES.find(([s]) => s === live.size) || BENCH_SIZES[0])[2];
    return sum + (per ? Math.min(per, BENCH_TOTALS[id] || per) : BENCH_TOTALS[id] || 0);
  }, 0);
  const total = live.models.length * each;
  const seconds = Math.max(1, Math.round((live.running || !live.ended ? Date.now() / 1000 : live.ended) - (live.started || 0)));
  // At the pace of the model answering now, and the same for those after it.
  // The whole run's pace had a quick model before a slow one say minutes for
  // what took hours.
  const on = live.models.find((m) => m.state === 'running' && m.started);
  const pace = on && doneOf(on) > 4 ? (Date.now() / 1000 - on.started) / doneOf(on) : 0;
  const after = live.models.slice(live.models.indexOf(on) + 1).filter((m) => m.state === 'waiting').length;
  const left = live.running && pace ? Math.round(pace * (Math.max(0, each - doneOf(on)) + after * each)) : 0;
  const rows = live.models.map((m) => {
    const benches = live.benchmarks.map((id) => {
      const p = (m.benchmarks || {})[id];
      const name = (BENCH_SUITE.find(([x]) => x === id) || [id, id])[1];
      if (!p) return `<div class="bench-bar waiting"><span class="name">${escape(name)}</span><span class="bar"><span style="width:0"></span></span><span class="status"></span></div>`;
      const share = p.n ? p.done / p.n : 0;
      return `<div class="bench-bar${m.current === id ? ' here' : ''}"><span class="name">${escape(name)}</span>
          <span class="bar"><span style="width:${(share * 100).toFixed(1)}%"></span></span>
          <span class="status">${p.error ? `<span class="bad" title="${escape(p.error)}">${escape(p.error)}</span>`
                                         : p.done ? `${pct(p.correct / p.done)}%` : ''}</span></div>`;
    }).join('');
    return `<div class="bench-live-model">
        <div class="row"><strong>${escape(m.name ? fileName(m.name) : '')}</strong>
          <span class="status">${escape(m.state)}${m.overall !== undefined && m.overall !== null ? `, overall ${pct(m.overall)}` : ''}</span></div>
        ${m.error ? `<div class="bad">${escape(m.error)}</div>` : ''}
        <div class="bench-bars">${benches}</div></div>`;
  }).join('');
  return `<div class="bench-live">
      <div class="row"><div class="caption" style="margin:0;flex:1">${live.running ? 'RUNNING' : live.stopped ? 'STOPPED' : 'LAST RUN'}</div>
        <span class="status">${apart(live.fetching ? `Fetching ${escape(benchTitle(live.fetching))}` : '',
          `${done} of ${total} questions`, span(seconds), left ? `about ${span(left)} left` : '')}</span></div>
      ${live.error ? `<div class="bad">${escape(live.error)}</div>` : ''}${rows}</div>`;
}

function benchCompareView() {
  const b = state.bench;
  if (!b.compare.length) {
    return '<p class="lede">Pick up to three.</p>';
  }
  const rows = b.compare.map((c) => rowOf(c.entry, { color: c.color, key: c.key, note: c.board ? 'board' : '' }));
  return `<div class="bench-compare">
      <div class="radar-wrap">${radarSvg(rows.map((x) => ({ label: x.label, color: x.color, skills: x.skills })))}</div>
      <div class="radar-legend">${rows.map((x) => `<div class="legend-row">
          <span class="key-line" style="background:${x.color}"></span><span class="legend-name" title="${escape(x.title || x.label)}">${escape(x.label)}</span>
          <button class="icon small" data-act="bench-uncompare" data-key="${escape(x.key)}" aria-label="Take ${escape(x.label)} off the chart">${CODE_ICONS.close}</button></div>`).join('')}</div>
    </div>${compareTable(rows, true)}`;
}

function benchResultsView() {
  const results = state.bench.results;
  if (!results) return '<div class="status">Reading...</div>';
  if (!results.results.length) return '<p class="lede">No results yet.</p>';
  return `<div class="bench-results">${results.results.map((r) => {
      const row = rowOf(r);
      const shown = state.bench.compare.some((c) => c.key === r.id);
      return `<div class="card bench-result">
        <div class="title"><strong>${escape(row.label)}</strong>
          <span class="tag">${escape(r.size)}</span>${r.stopped ? '<span class="tag">stopped</span>'
                                                     : row.partial ? '<span class="tag">partial</span>' : ''}${
            benchCut(r) ? `<span class="tag" title="Answers that ran out of room">${benchCut(r)} cut off</span>` : ''}${
            r.settings && r.settings.effort && r.settings.effort !== 'default'
              ? `<span class="tag">${escape(r.settings.effort)} effort</span>` : ''}
          <span class="spacer"></span>
          <span class="bench-overall">${row.overall === null ? '' : pct(row.overall)}</span></div>
        <div class="hint">${apart(escape(r.date || ''), r.speed && r.speed.gen_tps ? `${r.speed.gen_tps} tok/s` : '',
          r.seconds ? span(r.seconds) : '', escape((r.hardware && r.hardware.backend) || ''))}</div>
        <div class="row" style="margin-top:.5rem">
          <button class="action small" data-act="bench-compare" data-id="${escape(r.id)}" ${shown ? 'disabled' : ''}>Compare</button>
          <button class="action small" data-act="bench-detail" data-id="${escape(r.id)}">Details</button>
          <button class="action small" data-act="bench-post" data-id="${escape(r.id)}"
                  ${Object.keys(r.benchmarks || {}).length ? '' : 'disabled title="Nothing finished to post"'}>Post</button>
          <button class="link small" data-act="bench-delete" data-id="${escape(r.id)}">Delete</button></div>
      </div>`;
    }).join('')}</div>`;
}

function benchBoardView() {
  const b = state.bench;
  const board = b.board;
  if (!board) return '<div class="status">Reading...</div>';
  const query = b.boardQuery.trim().toLowerCase();
  const entries = (board.entries || []).filter((e) => e && typeof e === 'object' && !Array.isArray(e))
    .filter((e) => !query
    || `${(e.model || {}).repo || ''} ${(e.model || {}).file || ''} ${(e.model || {}).quant || ''}`.toLowerCase().includes(query));
  const rows = entries.slice(0, 100).map((e) => {
    const row = rowOf(e);
    const key = `board-${e.id}`;
    const shown = b.compare.some((c) => c.key === key);
    return `<tr>
        <td class="model-cell"><span title="${escape(row.title)}">${escape(row.label)}</span>${
          row.partial ? ' <span class="status">partial</span>' : ''}</td>
        <td>${row.overall === null ? '' : pct(row.overall)}</td>
        ${BENCH_SKILLS.map(([skill]) => `<td>${row.skills[skill] === undefined ? '' : pct(row.skills[skill])}</td>`).join('')}
        <td>${escape(row.size)}</td><td>${escape(((e.posted || {}).user) || '')}</td>
        <td><button class="action small" data-act="bench-compare-board" data-id="${escape(String(e.id))}" ${shown ? 'disabled' : ''}>Compare</button></td></tr>`;
  }).join('');
  return `<div class="row" style="margin-bottom:.6rem">
      <input id="board-query" data-draft data-input="board-typed" value="${escape(b.boardQuery)}" placeholder="Filter models"
             aria-label="Filter the board" autocomplete="off" spellcheck="false" style="max-width:18rem">
      <span class="spacer"></span>
      <span class="status">${board.offline ? 'offline copy' : count((board.entries || []).length, 'result')}</span>
      <button class="action small" data-act="bench-board-refresh">Refresh</button></div>
    ${rows ? `<table class="bench-table board"><thead><tr><th>Model</th><th>Overall</th>${
        BENCH_SKILLS.map(([, name]) => `<th>${name}</th>`).join('')}<th>Size</th><th>By</th><th></th></tr></thead><tbody>${rows}</tbody></table>`
      : `<p class="lede">${(board.entries || []).length ? 'Nothing matches.' : 'Nobody has posted yet.'}</p>`}`;
}

views.bench = () => {
  const b = state.bench;
  const live = state.snapshot.bench;
  return sideView() + `<div class="pane"><div class="settings-page bench">
      <h1>Bench</h1>
      ${b.open ? benchDetailView(b.open) : `
      ${benchRunView()}
      ${live ? benchLiveView() : ''}
      <h2 style="margin-top:2rem">COMPARE</h2>${benchCompareView()}
      <h2 style="margin-top:2rem">RESULTS</h2>${benchResultsView()}
      <h2 style="margin-top:2rem">PUBLIC BOARD</h2>${benchBoardView()}`}
    </div></div>`;
};

/// What the checker read from a wrong answer, said shortly: the letter or the
/// number it found, how many instructions were followed, or that code failed.
function benchRead(id, item) {
  const read = item.read;
  if (id === 'ifeval' && Array.isArray(read)) return `${read.filter(Boolean).length} of ${read.length} followed`;
  if (id === 'humaneval' || id === 'mbpp') return 'failed';
  if (read === null || read === undefined || read === '') return 'no answer';
  return `read ${Array.isArray(read) ? read.join(', ') : read}`;
}

/// One result in full: every benchmark with its score, how sure that score
/// is at this many items, and what the model said where it was wrong.
function benchDetailView(r) {
  const rows = BENCH_SUITE.map(([id, name]) => {
    const counts = (r.benchmarks || {})[id];
    const items = (r.items || {})[id] || [];
    const missed = (r.errors || {})[id];
    if (!counts && !items.length) {
      return missed ? `<tr><td>${escape(name)}</td><td colspan="3" class="bad wrap">${escape(missed)}</td></tr>` : '';
    }
    const n = counts ? counts.n : items.length;
    const right = counts ? counts.correct : items.filter((x) => x.ok).length;
    const p = n ? right / n : 0;
    // A 95% interval: what a score from fifty items can and cannot say.
    const margin = n ? 1.96 * Math.sqrt((p * (1 - p)) / n) : 0;
    const wrong = items.filter((x) => !x.ok).slice(0, 3);
    const cut = Number((r.cut_off || {})[id]) || 0;
    return `<tr><td>${escape(name)}</td><td>${pct(p)}</td><td class="status">&plusmn;${pct(margin)}</td>
        <td>${right} of ${n}${counts ? '' : ' (stopped)'}${cut ? ` <span class="status">${cut} cut off</span>` : ''}</td></tr>${wrong.length ? `<tr class="wrong-row"><td colspan="4">${
          wrong.map((x) => `<details><summary>${apart(`#${x.i}`, escape(benchRead(id, x)), x.limit ? 'hit the limit' : '')}</summary>
            ${/^(humaneval|mbpp)$/.test(id) && x.read ? codeBlock(String(x.read), 'text') : ''}
            ${codeBlock(String(x.answer || ''), 'text')}</details>`).join('')}</td></tr>` : ''}`;
  }).join('');
  const row = rowOf(r);
  return `<button class="link" data-act="bench-close">&larr; Bench</button>
    <div class="row" style="margin-top:.6rem"><h1 style="flex:1">${escape(row.label)}</h1>
      <span class="bench-overall">${row.overall === null ? '' : pct(row.overall)}</span></div>
    <div class="hint">${apart(escape(r.size), escape(r.date || ''), r.speed && r.speed.gen_tps ? `${r.speed.gen_tps} tok/s` : '',
      r.speed && r.speed.prompt_tps ? `${r.speed.prompt_tps} tok/s reading` : '', escape((r.model || {}).file || ''))}</div>
    <div class="radar-wrap solo">${radarSvg([{ label: row.label, color: BENCH_COLORS[0], skills: row.skills }])}</div>
    <table class="bench-table detail"><thead><tr><th>Benchmark</th><th>Score</th><th>95%</th><th>Right</th></tr></thead>
      <tbody>${rows}</tbody></table>`;
}

// --- what is fetched, and when ------------------------------------------------------------

const loadBenchResults = () => call('bench.results').then((got) => { state.bench.results = got; })
  .catch((error) => { state.error = error.message; }).finally(render);
const loadBoard = (refresh) => call('bench.board', { refresh: !!refresh }).then((got) => { state.bench.board = got; })
  .catch(() => { state.bench.board = { entries: [], offline: true }; }).finally(render);

entering.bench = () => {
  // Read again: a model condensed or downloaded since is one to run.
  need('models', 'models', true);
  if (!state.bench.results) loadBenchResults();
  if (!state.bench.board) loadBoard(false);
};

/// A run that has just ended leaves a result: the list is read again.
let benchWasRunning = false;
onSnapshot.push(() => {
  const running = !!(state.snapshot.bench && state.snapshot.bench.running);
  if (benchWasRunning && !running) loadBenchResults();
  benchWasRunning = running;
});

// --- the actions ------------------------------------------------------------------------

actions['bench-model'] = (button) => {
  const picked = state.bench.picked;
  picked[button.dataset.path] = !picked[button.dataset.path];
  render();
};
actions['bench-size'] = (button) => { state.bench.size = button.dataset.size; render(); };
actions['bench-effort'] = (button) => { state.bench.effort = button.dataset.effort; render(); };
actions['bench-toggle'] = (button) => {
  const off = state.bench.off;
  off[button.dataset.id] = !off[button.dataset.id];
  // Not every benchmark left out: a run of nothing is no run.
  if (BENCH_SUITE.every(([id]) => off[id])) off[button.dataset.id] = false;
  render();
};
actions['bench-start'] = () => guard(async () => {
  const b = state.bench;
  const models = (state.models ? state.models.models || [] : []).filter((m) => b.picked[m.path])
    .map((m) => ({ path: m.path, name: fileName(m.name) }));
  await call('bench.start', { models, size: b.size, effort: b.effort,
                              benchmarks: BENCH_SUITE.map(([id]) => id).filter((id) => !b.off[id]) });
});
actions['bench-stop'] = () => guard(() => call('bench.stop'));

/// Put a result on the chart. Three at most; a fourth takes the place of the
/// oldest, and nobody else's color changes.
function benchCompare(key, entry, board) {
  const b = state.bench;
  if (b.compare.some((c) => c.key === key)) return;
  if (b.compare.length >= BENCH_COLORS.length) b.compare.shift();
  const used = b.compare.map((c) => c.color);
  const color = BENCH_COLORS.find((x) => !used.includes(x)) || BENCH_COLORS[0];
  b.compare.push({ key, color, entry, board: !!board });
  render();
}
actions['bench-compare'] = (button) => {
  const r = (state.bench.results.results || []).find((x) => x.id === button.dataset.id);
  if (r) benchCompare(r.id, r, false);
};
actions['bench-compare-board'] = (button) => {
  const e = ((state.bench.board || {}).entries || []).find((x) => String(x.id) === button.dataset.id);
  if (e) benchCompare(`board-${e.id}`, e, true);
};
actions['bench-uncompare'] = (button) => {
  state.bench.compare = state.bench.compare.filter((c) => c.key !== button.dataset.key);
  render();
};
/// The page starts at its top when what it shows changes, not wherever the
/// list had been scrolled to.
const benchTop = () => { const page = document.querySelector('.settings-page.bench'); if (page) page.scrollTop = 0; };
actions['bench-detail'] = (button) => guard(async () => {
  state.bench.open = await call('bench.result', { id: button.dataset.id });
  benchTop();
});
actions['bench-close'] = () => { state.bench.open = null; benchTop(); render(); };
actions['bench-delete'] = async (button) => {
  const sure = await confirmIt({ title: 'Delete this result?', body: 'Posted copies stay.', yes: 'Delete', no: 'Keep' });
  if (!sure) return;
  await guard(async () => {
    await call('bench.delete', { id: button.dataset.id });
    state.bench.compare = state.bench.compare.filter((c) => c.key !== button.dataset.id);
    await loadBenchResults();
  });
};
actions['board-typed'] = (input) => { state.bench.boardQuery = input.value; render(); };
actions['bench-board-refresh'] = () => { state.bench.board = null; render(); loadBoard(true); };

// --- posting to the board ---------------------------------------------------------------
// Only a model file that is public on Hugging Face can go: the core looks for
// it there by its hash before the dialog shows what would be posted.

modals['bench-post'] = (m) => `<div class="modal wide">
    <div class="head"><strong>Post to the public board</strong>
      <div class="status">${m.gh ? `A public issue on ${escape(m.repo)}, from your GitHub account.`
                                 : `Opens ${escape(m.repo)} on GitHub.`}</div></div>
    <div class="body-pad">
      ${m.posting ? codeBlock(JSON.stringify(m.posting, null, 2), 'json') : '<div class="status">Reading...</div>'}
      ${m.posting && !m.public ? `<div class="bad" style="margin-top:.6rem">${
        m.offline ? 'Hugging Face unreachable' : 'Not on Hugging Face'}</div>` : ''}
      ${m.error ? `<div class="bad" style="margin-top:.6rem">${escape(m.error)}</div>` : ''}
      ${m.url ? `<div class="hint" style="margin-top:.6rem">${m.posted ? 'Posted: ' : 'Opened: '}${escape(m.url.length > 90 ? m.url.slice(0, 90) + '...' : m.url)}</div>` : ''}
    </div>
    <div class="feet">
      ${m.url ? '<span class="spacer"></span><button class="action" data-act="modal-close">Done</button>'
              : `<button class="action" data-act="modal-close">Cancel</button>
                 <span class="spacer"></span>
                 <button class="action primary" data-act="bench-post-go" ${m.posting && m.public && !m.posting_now ? '' : 'disabled'}>${
                  m.posting_now ? 'Posting...' : 'Post'}</button>`}
    </div></div>`;

actions['bench-post'] = (button) => guard(async () => {
  const id = button.dataset.id;
  openModal({ kind: 'bench-post', id, posting: null, repo: 'mattsaund/crucible-bench', gh: false });
  const got = await call('bench.posting', { id });
  if (state.modal && state.modal.kind === 'bench-post' && state.modal.id === id) {
    Object.assign(state.modal, got);
  }
});
actions['bench-post-go'] = () => guard(async () => {
  const m = state.modal;
  m.posting_now = true;
  m.error = '';
  render();
  try {
    const got = await call('bench.post', { id: m.id });
    m.url = got.url;
    m.posted = !!got.posted;
    if (got.posted) loadBoard(true);
  } catch (error) {
    m.error = error.message;
  } finally {
    m.posting_now = false;
  }
});
