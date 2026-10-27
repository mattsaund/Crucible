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
// So the tab opens on the board: its best models, each a click from its
// download in the Models tab, the chart, and every result to browse and
// open, this machine's own among them. Running a model and posting what it
// scored is the screen behind Add benchmark, in the corner.
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
  adding: false,                // on the screen a run is set up and posted from
  seeded: false,                // the board's best put on the chart, once
  versus: null,                 // { paths }: a model made in Create and its base, run against each other
};

/// How many of the board's best models the tab opens on.
const BENCH_BEST = 5;

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

// --- the board, and this machine beside it ----------------------------------------------

/// The board's entries that are results at all: an object each. Anything
/// else in the list is skipped rather than taking the page down.
function benchEntries() {
  const entries = (state.bench.board || {}).entries;
  return (Array.isArray(entries) ? entries : []).filter((e) => e && typeof e === 'object' && !Array.isArray(e));
}

/// An entry's repository on Hugging Face, when it is one the Models tab can
/// open: "owner/name" and nothing more.
function benchRepo(e) {
  const repo = String((e.model || {}).repo || '');
  return /^[A-Za-z0-9][A-Za-z0-9._-]*\/[A-Za-z0-9][A-Za-z0-9._-]*$/.test(repo) ? repo : '';
}

/// Whether an entry's model file is on this machine already: a file of its
/// name and size in the models folder.
function benchHere(e) {
  const model = e.model || {};
  return !!model.file && (state.models ? state.models.models || [] : [])
    .some((m) => fileName(m.name) === model.file && m.bytes === model.bytes);
}

/// The board's best models, each once, best first. A model's overall is that
/// of its widest run of every skill, a full run before a quick one: an overall
/// of fewer skills is another measure, and fifty items say less than all.
function benchBest(entries) {
  const asked = (e) => BENCH_SIZES.findIndex(([id]) => id === e.size);
  const best = new Map();
  for (const entry of entries) {
    const row = rowOf(entry);
    const repo = benchRepo(entry);
    if (row.partial || row.overall === null || !repo || !(entry.model || {}).file) continue;
    const key = String(entry.model.sha256 || `${repo}/${entry.model.file}`);
    const had = best.get(key);
    if (!had || asked(entry) > asked(had.entry)
        || (asked(entry) === asked(had.entry) && row.overall > had.row.overall)) best.set(key, { entry, row });
  }
  return [...best.values()].sort((a, b) => b.row.overall - a.row.overall);
}

/// One run, whoever holds it: the file, the size and every count. A result
/// kept here that is on the board already is the board's row and not a
/// second one.
const benchRun = (e) => JSON.stringify([String((e.model || {}).sha256 || ''), String(e.size || ''),
  BENCH_SUITE.map(([id]) => { const c = (e.benchmarks || {})[id]; return c ? [c.n, c.correct] : 0; })]);

/// Every result to browse: the board's and this machine's own, ordered as the
/// board orders itself, every skill before fewer and then the best overall.
function benchDirectory() {
  const board = benchEntries();
  const posted = new Set(board.map(benchRun));
  const mine = ((state.bench.results || {}).results || [])
    .filter((r) => Object.keys(r.benchmarks || {}).length && !posted.has(benchRun(r)));
  return board.map((entry) => ({ entry, key: `board-${entry.id}`, mine: false }))
    .concat(mine.map((entry) => ({ entry, key: entry.id, mine: true })))
    .map((x) => Object.assign(x, { row: rowOf(x.entry) }))
    .sort((a, b) => (a.row.partial - b.row.partial)
      || ((b.row.overall === null ? -1 : b.row.overall) - (a.row.overall === null ? -1 : a.row.overall)));
}

/// What a search looks through: the model's names, who posted it, the
/// machine and the size. "you" finds this machine's own.
function benchSought(x) {
  const e = x.entry;
  const model = e.model || {};
  return [x.row.label, model.repo, model.file, model.quant, model.arch, (e.posted || {}).user,
          (e.hardware || {}).backend, e.size, x.mine ? 'you' : '']
    .map((part) => String(part || '')).join(' ').toLowerCase();
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
  const rows = b.compare.map((c) => rowOf(c.entry, { color: c.color, key: c.key,
                                                     note: c.board ? 'board' : c.entry.made || '' }));
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
          <span class="tag">${escape(r.size)}</span>${r.made ? `<span class="tag">${escape(r.made)}</span>` : ''}${
            r.stopped ? '<span class="tag">stopped</span>' : row.partial ? '<span class="tag">partial</span>' : ''}${
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
          ${benchPostButton(r)}
          <button class="link small" data-act="bench-delete" data-id="${escape(r.id)}">Delete</button></div>
      </div>`;
    }).join('')}</div>`;
}

/// Post, for a result kept here. A model made in Create is on nobody's
/// Hugging Face, which is the only place the board takes a model from, so
/// the button says that instead of offering a post that would be refused.
function benchPostButton(r) {
  if (r.made) return '<button class="action small" disabled>Not public</button>';
  return `<button class="action small" data-act="bench-post" data-id="${escape(r.id)}"
      ${Object.keys(r.benchmarks || {}).length ? '' : 'disabled title="Nothing finished to post"'}>Post</button>`;
}

/// The board's best models, near the top, ranked: each with its overall and a
/// way to its download in the Models tab, unless it is here already.
function benchBestView() {
  const b = state.bench;
  const best = benchBest(benchEntries()).slice(0, BENCH_BEST);
  if (!best.length) return '';
  return `<h2 style="margin-top:1.4rem">TOP MODELS</h2>
    <div class="order bench-best">${best.map(({ entry: e, row }, i) => {
      const id = escape(String(e.id ?? ''));
      const shown = b.compare.some((c) => c.key === `board-${e.id}`);
      return `<div class="order-row">
          <span class="order-n">${i + 1}</span>
          ${benchRepo(e) ? repoLogo({ id: benchRepo(e) }) : '<span class="logo" aria-hidden="true"></span>'}
          <span class="order-name"><button class="bench-name" data-act="bench-open-board" data-id="${id}"
              title="${escape(row.title)}">${escape(row.label)}</button>
            <span class="status">${apart(escape(benchRepo(e).split('/')[0]),
              escape(paramsLabel(Number(e.model.params) / 1e9)), escape(e.size || ''))}</span></span>
          <span class="bench-overall">${pct(row.overall)}</span>
          ${benchHere(e) ? '<span class="status here">here</span>'
            : `<button class="action" data-act="bench-get" data-id="${id}">Download</button>`}
          <button class="action" data-act="bench-compare-board" data-id="${id}" ${shown ? 'disabled' : ''}>Compare</button>
        </div>`;
    }).join('')}</div>`;
}

/// Every result there is to look at, the board's and this machine's, as one
/// directory: searched, opened, put on the chart. Without the board, offline
/// and never posted to, the results kept here are still here.
function benchBoardView() {
  const b = state.bench;
  const board = b.board;
  if (!board && !b.results) return '<div class="status">Reading...</div>';
  const query = b.boardQuery.trim().toLowerCase();
  const all = benchDirectory();
  const rows = all.filter((x) => !query || benchSought(x).includes(query)).slice(0, 100).map((x) => {
    const e = x.entry;
    const id = escape(String(e.id ?? ''));
    const shown = b.compare.some((c) => c.key === x.key);
    return `<tr>
        <td class="model-cell"><button class="bench-name" data-act="${x.mine ? 'bench-detail' : 'bench-open-board'}"
            data-id="${id}" title="${escape(x.row.title)}">${escape(x.row.label)}</button>${
          x.mine && e.made ? ` <span class="status">${escape(e.made)}</span>` : ''}${
          x.row.partial ? ' <span class="status">partial</span>' : ''}</td>
        <td>${x.row.overall === null ? '' : pct(x.row.overall)}</td>
        ${BENCH_SKILLS.map(([skill]) => `<td>${x.row.skills[skill] === undefined ? '' : pct(x.row.skills[skill])}</td>`).join('')}
        <td>${escape(x.row.size)}</td><td>${x.mine ? 'you'
          : `<span class="by" title="${escape((e.posted || {}).user || '')}">${escape((e.posted || {}).user || '')}</span>`}</td>
        <td><button class="action small" data-act="${x.mine ? 'bench-compare' : 'bench-compare-board'}" data-id="${id}"
                    ${shown ? 'disabled' : ''}>Compare</button></td></tr>`;
  }).join('');
  const posted = benchEntries().length;
  // Said in a word where the count goes: no board, or the copy from last time.
  const status = !board ? 'Reading...' : board.offline ? (posted ? 'offline copy' : 'offline') : count(posted, 'result');
  return `<div class="row" style="margin-bottom:.6rem">
      <input id="board-query" data-draft data-input="board-typed" value="${escape(b.boardQuery)}" placeholder="Search"
             aria-label="Search the results" autocomplete="off" spellcheck="false" style="max-width:18rem">
      <span class="spacer"></span>
      <span class="status">${status}</span>
      <button class="action small" data-act="bench-board-refresh">Refresh</button></div>
    ${rows ? `<table class="bench-table board"><thead><tr><th>Model</th><th>Overall</th>${
        BENCH_SKILLS.map(([, name]) => `<th>${name}</th>`).join('')}<th>Size</th><th>By</th><th></th></tr></thead><tbody>${rows}</tbody></table>`
      : `<p class="lede">${query && all.length ? 'Nothing matches.'
          : board && board.offline ? 'Board offline.' : board ? 'Nobody has posted yet.' : ''}</p>`}`;
}

/// What the tab opens on: the board's best, the chart, and every result,
/// with the way to add one in the corner.
function benchHomeView() {
  const live = state.snapshot.bench;
  return `<div class="row bench-head"><h1 style="flex:1">Bench</h1>
      ${live && live.running ? '<span class="working">Running</span>' : ''}
      <button class="action primary" data-act="bench-add">Add benchmark</button></div>
    ${benchBestView()}
    <h2 style="margin-top:2rem">COMPARE</h2>${benchCompareView()}
    <h2 style="margin-top:2rem">RESULTS</h2>${benchBoardView()}`;
}

/// Running models through the suite, how the run is going, and the results
/// kept here, from where each can go to the board.
function benchAddView() {
  const live = state.snapshot.bench;
  return `<button class="link" data-act="bench-home">&larr; Bench</button>
    <h1 style="margin-top:.6rem">Add benchmark</h1>
    ${benchRunView()}
    ${live ? benchLiveView() : ''}
    <h2 style="margin-top:2rem">COMPARE</h2>${benchCompareView()}
    <h2 style="margin-top:2rem">YOURS</h2>${benchResultsView()}`;
}

views.bench = () => {
  const b = state.bench;
  return sideView() + `<div class="pane"><div class="settings-page bench">${
    b.open ? benchDetailView(b.open) : b.adding ? benchAddView() : benchHomeView()}</div></div>`;
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

/// One result in full, kept here or from the board: every benchmark with its
/// score and how sure that score is at this many items, what the model is,
/// what it ran on and with, and for one run here what the model said where it
/// was wrong. Everything a board entry holds is somebody else's, as text.
function benchDetailView(r) {
  const b = state.bench;
  const rows = BENCH_SUITE.map(([id, name]) => {
    const counts = (r.benchmarks || {})[id];
    const kept = (r.items || {})[id];
    const items = Array.isArray(kept) ? kept : [];
    const missed = (r.errors || {})[id];
    if (!counts && !items.length) {
      return missed ? `<tr><td>${escape(name)}</td><td colspan="3" class="bad wrap">${escape(missed)}</td></tr>` : '';
    }
    const n = counts ? Number(counts.n) || 0 : items.length;
    const right = counts ? Number(counts.correct) || 0 : items.filter((x) => x.ok).length;
    const p = n ? right / n : 0;
    // A 95% interval: what a score from fifty items can and cannot say.
    const margin = n ? 1.96 * Math.sqrt((p * (1 - p)) / n) : 0;
    const wrong = items.filter((x) => !x.ok).slice(0, 3);
    const cut = Number((r.cut_off || {})[id]) || 0;
    // An answer that ran out of room by going round in circles says so: the
    // runner stops a reply that loops, and that is not a long answer.
    return `<tr><td>${escape(name)}</td><td>${pct(p)}</td><td class="status">&plusmn;${pct(margin)}</td>
        <td>${right} of ${n}${counts ? '' : ' (stopped)'}${cut ? ` <span class="status">${cut} cut off</span>` : ''}</td></tr>${wrong.length ? `<tr class="wrong-row"><td colspan="4">${
          wrong.map((x) => `<details><summary>${apart(`#${escape(x.i)}`, escape(benchRead(id, x)),
              x.looped ? 'looped' : x.limit ? 'hit the limit' : '')}</summary>
            ${/^(humaneval|mbpp)$/.test(id) && x.read ? codeBlock(String(x.read), 'text') : ''}
            ${codeBlock(String(x.answer || ''), 'text')}</details>`).join('')}</td></tr>` : ''}`;
  }).join('');
  const row = rowOf(r);
  const model = r.model && typeof r.model === 'object' ? r.model : {};
  const repo = r.board ? benchRepo(r) : '';
  const facts = [['REPO', repo], ['FILE', model.file], ['QUANT', model.quant], ['ARCH', model.arch],
                 ['PARAMS', paramsLabel(Number(model.params) / 1e9)]]
    .filter(([, value]) => value)
    .map(([key, value]) => `<span class="fact"><span class="caption">${key}</span> ${escape(value)}</span>`).join('');
  const id = escape(String(r.id ?? ''));
  const shown = b.compare.some((c) => c.key === (r.board ? `board-${r.id}` : r.id));
  const doing = r.board
    ? `<button class="action small" data-act="bench-compare-board" data-id="${id}" ${shown ? 'disabled' : ''}>Compare</button>
       ${benchHere(r) ? '<span class="status">here</span>'
         : repo ? `<button class="action small" data-act="bench-get" data-id="${id}">Download</button>` : ''}`
    : `<button class="action small" data-act="bench-compare" data-id="${id}" ${shown ? 'disabled' : ''}>Compare</button>
       ${benchPostButton(r)}`;
  return `<button class="link" data-act="bench-close">&larr; Bench</button>
    <div class="row" style="margin-top:.6rem"><h1 style="flex:1">${escape(row.label)}</h1>
      <span class="bench-overall">${row.overall === null ? '' : pct(row.overall)}</span></div>
    <div class="hint">${apart(escape(r.size || ''), escape(r.date || ''),
      r.board ? escape((r.posted || {}).user || '') : 'you', r.made ? escape(r.made) : '',
      r.stopped ? 'stopped' : row.partial ? 'partial' : '')}</div>
    ${facts ? `<div class="facts">${facts}</div>` : ''}
    <div class="row">${doing}</div>
    <div class="bench-compare bench-one">
      <div class="radar-wrap solo">${radarSvg([{ label: row.label, color: BENCH_COLORS[0], skills: row.skills }])}</div>
      <table class="bench-table detail"><thead><tr><th>Benchmark</th><th>Score</th><th>95%</th><th>Right</th></tr></thead>
        <tbody>${rows}</tbody></table></div>
    <h2 style="margin-top:1.6rem">SETUP</h2>${benchSpecs(r)}`;
}

/// What a result ran on and with: the machine, the settings every item had,
/// how fast it went, and which Crucible and llama.cpp. A board entry has the
/// same, but for the cards' names, which stay on the machine that ran it.
function benchSpecs(r) {
  const part = (x) => (x && typeof x === 'object' && !Array.isArray(x) ? x : {});
  const number = (x) => (typeof x === 'number' && Number.isFinite(x) ? x : null);
  const hardware = part(r.hardware);
  const settings = part(r.settings);
  const speed = part(r.speed);
  const cards = Array.isArray(hardware.gpus) ? hardware.gpus.map((g) => escape(g)).join(', ') : '';
  const spec = (label, value) => (value ? `<div><span>${label}</span><span>${value}</span></div>` : '');
  return `<div class="runtime-detail" style="border:0;padding:0;margin:0">
      ${spec('Backend', escape(hardware.backend || ''))}
      ${spec('GPUs', cards || (number(hardware.gpu_count) === null ? '' : String(hardware.gpu_count)))}
      ${spec('GPU memory', number(hardware.memory_gb) === null ? '' : `${hardware.memory_gb.toFixed(1)} GB`)}
      ${spec('Context', number(settings.context) ? `${settings.context} tokens` : '')}
      ${spec('Reply limit', number(settings.max_tokens) ? `${settings.max_tokens} tokens` : '')}
      ${spec('Temperature', number(settings.temperature) === null ? '' : String(settings.temperature))}
      ${spec('Effort', escape(settings.effort || ''))}
      ${spec('Writing', number(speed.gen_tps) === null ? '' : `${speed.gen_tps} tok/s`)}
      ${spec('Reading', number(speed.prompt_tps) === null ? '' : `${speed.prompt_tps} tok/s`)}
      ${spec('Took', number(r.seconds) ? span(r.seconds) : '')}
      ${spec('Crucible', escape(r.crucible || ''))}
      ${spec('llama.cpp', escape(r.llama || ''))}
    </div>`;
}

// --- what is fetched, and when ------------------------------------------------------------

const loadBenchResults = () => call('bench.results').then((got) => { state.bench.results = got; })
  .catch((error) => { state.error = error.message; }).finally(render);
const loadBoard = (refresh) => call('bench.board', { refresh: !!refresh })
  .then((got) => {
    state.bench.board = got;
    benchSeed();
    // The makers' logos beside the best, as the Models tab shows them.
    askLogos(benchBest(benchEntries()).slice(0, BENCH_BEST).map(({ entry }) => ({ id: benchRepo(entry) }))
      .filter((l) => l.id));
  })
  .catch(() => { state.bench.board = { entries: [], offline: true }; }).finally(render);

/// A chart from the start: the board's best three, the first time the board
/// has any, unless something is on the chart already. Once only, so a chart
/// emptied by hand stays empty.
function benchSeed() {
  const b = state.bench;
  const best = benchBest(benchEntries()).slice(0, BENCH_COLORS.length);
  if (b.seeded || !best.length) return;
  b.seeded = true;
  if (b.compare.length) return;
  for (const { entry } of best) benchCompare(`board-${entry.id}`, entry, true);
}

entering.bench = () => {
  // Read again: a model condensed or downloaded since is one to run.
  need('models', 'models', true);
  if (!state.bench.results) loadBenchResults();
  if (!state.bench.board) loadBoard(false);
};

/// A run that has just ended leaves a result: the list is read again. A run
/// of a model made in Create against its base puts the two on the chart.
let benchWasRunning = false;
onSnapshot.push(() => {
  const live = state.snapshot.bench;
  const running = !!(live && live.running);
  if (benchWasRunning && !running) {
    const versus = state.bench.versus;
    state.bench.versus = null;
    loadBenchResults().then(() => { if (versus) benchVersusShown(versus, live); });
  }
  benchWasRunning = running;
});

/// A model made in Create, run against the model it was made from: both at
/// the size and effort set here, on the screen that shows the run, and the
/// two on the chart when it ends. `made` and `base` are their files.
function benchVersus(made, base) {
  const b = state.bench;
  b.picked = { [made]: true, [base]: true };
  b.adding = true;
  b.open = null;
  b.versus = { paths: [made, base] };
  enter('bench');
  return guard(async () => {
    try {
      await call('bench.start', { models: [made, base].map((path) => ({ path, name: fileName(path) })),
                                  size: b.size, effort: b.effort,
                                  benchmarks: BENCH_SUITE.map(([id]) => id).filter((id) => !b.off[id]) });
    } catch (error) {
      b.versus = null;
      throw error;
    }
  });
}

/// The results that run left, on the chart in place of what was there, the
/// made model first: those with a benchmark finished. A run stopped before
/// any was leaves the chart alone.
function benchVersusShown(versus, live) {
  const ids = ((live && live.models) || []).filter((m) => versus.paths.includes(m.path) && m.result)
    .map((m) => m.result);
  const kept = (state.bench.results || {}).results || [];
  const results = ids.map((id) => kept.find((r) => r.id === id))
    .filter((r) => r && Object.keys(r.benchmarks || {}).length);
  if (!results.length) return;
  state.bench.compare = [];
  for (const r of results) benchCompare(r.id, r, false);
}

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
  const e = benchEntries().find((x) => String(x.id) === button.dataset.id);
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
actions['bench-add'] = () => { state.bench.adding = true; state.bench.open = null; benchTop(); render(); };
actions['bench-home'] = () => { state.bench.adding = false; state.bench.open = null; benchTop(); render(); };
/// A board entry opened in full. A copy, marked as the board's, and without
/// anything only a result kept here has: the board has no answers to show.
actions['bench-open-board'] = (button) => {
  const e = benchEntries().find((x) => String(x.id) === button.dataset.id);
  if (!e) return;
  state.bench.open = Object.assign({}, e, { board: true, items: null, errors: null, cut_off: null, made: null });
  benchTop();
  render();
};
/// The Models tab, on the repository a board entry's file is in, with that
/// file chosen to download.
actions['bench-get'] = (button) => {
  const e = benchEntries().find((x) => String(x.id) === button.dataset.id);
  const repo = e ? benchRepo(e) : '';
  if (repo) marketShow(repo, repo, String(e.model.file || ''));
};
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
        m.made ? 'Not public' : m.offline ? 'Hugging Face unreachable' : 'Not on Hugging Face'}</div>` : ''}
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
