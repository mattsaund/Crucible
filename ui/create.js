// SPDX-License-Identifier: MIT
//
// Create: making an expert of your own.
//
// The tab is a list of experts before it is a form: what has been made, what
// each one is, and where it got to. Making one is a recipe -- a base model,
// some data, a target -- filled in a step at a time and then run.

const STAGES = [
  ['finished', 'FINISHED'],
  ['testing',  'READY TO TEST'],
  ['training', 'TRAINING'],
  ['draft',    'DRAFTS'],
];

const METHODS = [
  ['qlora', 'QLoRA', '4-bit base. Least memory.'],
  ['lora',  'LoRA',  'Full base. More memory.'],
];
const QUANTIZATIONS = ['Q4_K_M', 'Q5_K_M', 'Q6_K', 'Q8_0', 'F16'];
const WIZARD_STEPS = [
  ['Name',       ''],
  ['Base model', ''],
  ['Data',       'Questions and answers.'],
  ['Tools',      'Optional.'],
  ['Target',     ''],
  ['Review',     ''],
];

const recipesList = () => (state.recipes ? state.recipes.recipes : []);
const rate = (value) => Number(value).toExponential(1);

/// One line of what a recipe is: its base, its method, what comes out.
function specLine(r) {
  const parts = [];
  if (r.base && r.base.id) parts.push(r.base.label || fileName(r.base.id));
  if (r.parameters_b > 0) parts.push(`${r.parameters_b}B`);
  parts.push(r.method === 'lora' ? 'LoRA' : 'QLoRA');
  parts.push(`${r.format === 'mlx' ? 'MLX' : 'GGUF'} ${r.quantization}`);
  return parts.join('  ·  ');
}

// --- the list ---------------------------------------------------------------------------

function createList() {
  if (!state.recipes) return '<p class="lede">Reading...</p>';
  const all = recipesList();
  const groups = STAGES.map(([stage, title]) => {
    const rows = all.filter((r) => r.stage === stage);
    if (!rows.length) return '';
    return `<h2 style="margin-top:1.8rem">${title}</h2>` + rows.map((r) => `
      <div class="card pick" data-act="recipe-open" data-id="${escape(r.id)}">
        <div class="title"><strong>${escape(r.name || '(unnamed)')}</strong>
          <span class="tag">${escape(r.stage_text)}</span>
          ${r.trained_path ? `<button class="action right" data-act="recipe-test" data-id="${escape(r.id)}"
              ${r.trained_there ? '' : 'disabled title="Trained file missing"'}>Test</button>` : ''}</div>
        <div class="hint">${escape(r.purpose || 'no description')}</div>
        <div class="hint">${escape(specLine(r))}</div>
      </div>`).join('');
  }).join('');
  return `<h1>Create</h1>
    <button class="action" data-act="recipe-new">New expert</button>
    ${groups}`;
}

// --- one recipe ---------------------------------------------------------------------------

/// The loss, as a line: it should fall and then flatten, and a glance says
/// whether it is doing either.
function lossCurve(curve) {
  if (!curve || curve.length < 2) return '';
  const high = Math.max(...curve);
  const low = Math.min(...curve);
  const range = high - low || 1;
  const points = curve.map((value, i) =>
    `${((i / (curve.length - 1)) * 300).toFixed(1)},${(58 - ((value - low) / range) * 54).toFixed(1)}`).join(' ');
  return `<svg class="curve" viewBox="0 0 300 62" preserveAspectRatio="none" role="img"
      aria-label="Training loss, from ${high.toFixed(2)} to ${curve[curve.length - 1].toFixed(2)}">
    <polyline points="${points}"/></svg>
    <div class="status">loss ${curve[curve.length - 1].toFixed(3)}  ·  started at ${curve[0].toFixed(3)}</div>`;
}

/// What a run is doing, or how it ended.
function runView(r) {
  const run = state.run;
  const mine = run && run.recipe === r.id;
  if (mine && run.phase === 'running') {
    return `<div class="install">
        <div class="row"><strong>${escape(run.label)}</strong>
          <button class="action" data-act="run-stop" title="Can't be resumed">Stop the run</button></div>
        <div class="bar"><span style="width:${Math.round(run.percent * 100)}%"></span></div>
        <div class="status">${run.seconds_left > 0 ? `about ${span(run.seconds_left)} left`
                                                   : `running for ${span(run.seconds)}`}</div>
        <div class="runtime-detail">
          ${run.device ? `<div><span>On</span>${escape(run.device)}</div>` : ''}
          ${run.records ? `<div><span>Data</span>${count(run.records, 'record')}</div>` : ''}
          ${run.trainable ? `<div><span>Adapter</span>${(run.trainable / 1e6).toFixed(1)}M parameters</div>` : ''}
        </div>
        ${lossCurve(run.curve)}
        ${run.notes.map((n) => `<div class="status">${escape(n)}</div>`).join('')}
      </div>`;
  }
  if (mine && run.phase === 'failed') {
    return `<div class="install"><div class="bad">${escape(run.error)}</div>
        ${run.hint ? `<div class="hint">${escape(run.hint)}</div>` : ''}
        ${run.log.length ? `<details class="work" open><summary>the log</summary>
          <pre class="install-log">${escape(run.log.join('\n'))}</pre>
          <div class="status">${escape(run.log_file)}</div></details>` : ''}
        <div class="row" style="margin-top:.7rem">
          <button class="action" data-act="recipe-train" data-id="${escape(r.id)}">Try again</button>
          <button class="action" data-act="recipe-edit" data-id="${escape(r.id)}">Edit the recipe</button></div></div>`;
  }
  if (mine && run.phase === 'canceled') {
    return `<p class="lede">Stopped. Nothing was kept.</p>
      <button class="action" data-act="recipe-train" data-id="${escape(r.id)}">Start again</button>`;
  }
  // Marked as training, and nothing is. A run belongs to the window that
  // started it, so this is what a recipe looks like after that window closed.
  return `<p class="lede">Not running${r.started_at ? `. Started ${ago(r.started_at)}` : ''}.</p>
    <div class="row">
      <button class="action" data-act="recipe-train" data-id="${escape(r.id)}">Start again</button>
      <button class="action" data-act="recipe-draft" data-id="${escape(r.id)}">Put it back to a draft</button></div>
    <h2 style="margin-top:1.6rem">TRAINED ELSEWHERE</h2>
    <div class="row"><button class="action" data-act="recipe-attach" data-id="${escape(r.id)}">Choose the file</button></div>`;
}

function createDetail(r) {
  const trainer = state.trainer;
  const spec = (label, value, cls) =>
    `<div><span>${label}</span><span class="${cls || ''}">${value}</span></div>`;
  const fit = r.fit || {};
  const fits = !r.parameters_b ? ''
    : !fit.known ? spec('Fits', 'unknown', 'meh')
    : spec('Fits', `needs ${bytes(fit.needed)} of ${bytes(fit.have)}`,
           fit.possible ? '' : 'bad');
  const method = METHODS.find(([id]) => id === r.method) || METHODS[0];

  let stage;
  if (r.stage === 'draft') {
    const blocked = r.missing.length > 0;
    const noTrainer = trainer && !trainer.ready;
    stage = `<h2 style="margin-top:1.6rem">BEFORE IT CAN RUN</h2>
      <p class="lede${blocked ? ' bad' : ''}">${blocked ? 'still needs ' + r.missing.map(escape).join(', ')
                                                        : 'nothing missing'}</p>
      ${noTrainer ? '<p class="lede bad">Training is not set up.</p>' : ''}
      <div class="row">
        <button class="action" data-act="recipe-edit" data-id="${escape(r.id)}">Continue setup</button>
        ${noTrainer
          ? '<button class="action" data-act="settings-page" data-page="training">Set up training</button>'
          : `<button class="action" data-act="recipe-train" data-id="${escape(r.id)}" ${
               blocked ? 'disabled' : ''}>Start training</button>`}</div>`;
  } else if (r.stage === 'training') {
    stage = `<h2 style="margin-top:1.6rem">TRAINING</h2>${runView(r)}`;
  } else if (r.stage === 'testing') {
    stage = `<h2 style="margin-top:1.6rem">BEFORE YOU KEEP IT</h2>
      <div class="row">
        <button class="action" data-act="recipe-test" data-id="${escape(r.id)}" ${r.trained_there ? ''
          : 'disabled title="Trained file missing"'}>Test</button>
        <button class="action" data-act="recipe-edit" data-id="${escape(r.id)}" data-step="4">Edit</button></div>`;
  } else {
    stage = `<h2 style="margin-top:1.6rem">ON THE ROSTER</h2>
      <div class="row">
        <button class="action" data-act="recipe-test" data-id="${escape(r.id)}" ${
          r.trained_there ? '' : 'disabled'}>Test again</button>
        <button class="action" data-act="recipe-edit" data-id="${escape(r.id)}" data-step="4">Edit</button></div>`;
  }

  return `<button class="link" data-act="recipe-close">&larr; Create</button>
    <div class="row" style="margin-top:.6rem"><h1 style="flex:1">${escape(r.name || '(unnamed)')}
        <span class="tag">${escape(r.stage_text)}</span></h1>
      <button class="link" data-act="recipe-delete" data-id="${escape(r.id)}"
              data-name="${escape(r.name || 'this expert')}">Delete</button></div>
    <p class="lede">${escape(r.purpose || 'no description')}</p>
    <h2>SPECS</h2>
    <div class="runtime-detail" style="border:0;padding:0">
      ${spec('Base model', r.base.id ? escape(r.base.label || r.base.id) : 'not chosen', r.base.id ? '' : 'ok')}
      ${r.parameters_b ? spec('Size', `${r.parameters_b} billion parameters`) : ''}
      ${spec('Method', `${method[1]} <span class="status">${method[2]}</span>`)}
      ${spec('Data', r.data.length ? r.data.map((d) => escape(d.label || d.id)).join(', ') : 'none',
             r.data.length ? '' : 'ok')}
      ${r.tools.length ? spec('Tools', r.tools.map((d) => escape(d.label || d.id)).join(', ')) : ''}
      ${spec('Run', `${count(r.epochs, 'pass', 'passes')}  ·  ${r.context} tokens of context  ·  learning rate ${
        rate(r.learning_rate)}`)}
      ${spec('Export', `${r.format === 'mlx' ? 'MLX' : 'GGUF'} at ${escape(r.quantization)}${
        r.export_bytes ? ', about ' + bytes(r.export_bytes) : ''}`)}
      ${fits}
      ${r.trained_path ? spec('File', escape(r.trained_display || r.trained_path)
          + (r.trained_there ? `  ·  ${bytes(r.trained_bytes)}` : '  (not there)'),
          r.trained_there ? '' : 'bad') : ''}
      ${r.started_at ? spec('Started', ago(r.started_at)) : ''}
      ${r.finished_at ? spec('Finished', ago(r.finished_at)) : ''}
    </div>${stage}`;
}

views.create = () => {
  const open = state.open.recipe && recipesList().find((r) => r.id === state.open.recipe);
  return sideView() + `<div class="pane"><div class="settings-page">${
    open ? createDetail(open) : createList()}</div></div>`;
};

entering.create = () => {
  need('recipes', 'lab.recipes', true);
  need('trainer', 'trainer');
  need('run', 'lab.run', true);
};

const reloadRecipes = async () => { state.recipes = await call('lab.recipes'); };

actions['recipe-open']  = (e, event) => {
  if (event.target.closest('button')) return;   // the Test button on the card is its own thing
  state.open.recipe = e.dataset.id; render();
};
actions['recipe-close'] = () => { state.open.recipe = null; render(); };
actions['recipe-train'] = (e) => guard(async () => {
  await call('lab.train', { id: e.dataset.id });
  state.run = await call('lab.run');
  await reloadRecipes();
});
actions['run-stop'] = () => guard(async () => { state.run = await call('lab.run', { cancel: true }); });
actions['recipe-draft'] = (e) => guard(async () => {
  const r = recipesList().find((x) => x.id === e.dataset.id);
  await call('lab.save', { ...r, stage: 'draft' });
  await reloadRecipes();
});
actions['recipe-attach'] = async (e) => {
  const path = await pickPath({ title: 'Choose the trained model', filter: 'GGUF models',
                                extensions: ['.gguf'] });
  if (path) await guard(async () => { await call('lab.attach', { id: e.dataset.id, path }); await reloadRecipes(); });
};
actions['recipe-delete'] = async (e) => {
  const sure = await confirmIt({ title: 'Delete this expert?',
    body: `${e.dataset.name}: the recipe and what it trained.`,
    yes: 'Delete', no: 'Keep' });
  if (!sure) return;
  await guard(async () => {
    await call('lab.delete', { id: e.dataset.id });
    state.open.recipe = null;
    await reloadRecipes();
  });
};

/// Follow a run while one is going. The trainer pokes the window on every
/// step, which arrives as a snapshot, and this asks how it is doing.
let watchingRun = false;
onSnapshot.push(async () => {
  if (watchingRun || !state.run || state.run.phase !== 'running') return;
  watchingRun = true;
  try {
    state.run = await call('lab.run');
    if (state.run.phase !== 'running') await reloadRecipes();
  } catch (e) { /* asked again on the next snapshot */ }
  watchingRun = false;
  render();
});

// --- the wizard ------------------------------------------------------------------------------

const blankRecipe = () => ({ id: '', name: '', purpose: '', base: { source: 'hub', id: '' },
  data: [], tools: [], method: 'qlora', format: 'gguf', quantization: 'Q4_K_M',
  parameters_b: 0, epochs: 2, context: 512, learning_rate: 1e-5, stage: 'draft' });

const slugOf = (name) => name.toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-+|-+$/g, '');

/// Search Huggingface, and what came back. `slot` is which of the three
/// searches the wizard holds: the base model, the data, the tools.
function hubPicker(m, slot, kind) {
  const hub = m[slot];
  const rows = hub.searching ? '<div class="status">Searching...</div>'
    : hub.error ? `<div class="bad">${escape(hub.error)}</div>`
    : hub.asked && !hub.items.length ? '<div class="status">No matches.</div>'
    : hub.items.map((item) => `<div class="order-row">
        <span class="order-name">${escape(item.id)}
          <span class="status">${compact(item.downloads)} downloads${
            item.parameters_b ? `  ·  ${item.parameters_b}B` : ''}${
            item.gated ? '  ·  gated' : ''}</span></span>
        <button class="action" data-act="wz-use" data-slot="${slot}" data-id="${escape(item.id)}"
                data-params="${item.parameters_b || 0}">${kind === 'model' ? 'Use' : 'Add'}</button>
      </div>`).join('');
  return `<form class="row" data-submit="wz-search" data-slot="${slot}" data-hub="${kind}">
      <input data-draft data-input="wz-query" data-slot="${slot}" value="${escape(hub.query)}"
             placeholder="${kind === 'model' ? 'qwen, llama-3.2, smollm' : 'subject or name'}">
      <button class="action">Search</button></form>
    <div class="order" style="margin-top:.6rem">${rows}</div>`;
}

/// A file from this machine, typed or browsed for.
function localPicker(slot, label, filter, extensions, verb) {
  return `<div class="row">
      <input id="wz-local-${slot}" data-draft spellcheck="false" placeholder="${escape(label)}">
      <button type="button" class="action" data-act="wz-browse" data-slot="${slot}"
              data-filter="${escape(filter)}" data-ext="${escape(extensions.join(','))}">Browse</button>
      <button type="button" class="action" data-act="wz-local" data-slot="${slot}">${verb}</button></div>`;
}

/// What builds in the open project recorded of their agents' work, per
/// expert: the tasks a provider's model was given and what it made, as
/// chats. Offered first, because it is the data that teaches a local expert
/// this project, and it is already on the disk.
function teachPicker(r) {
  const records = state.teach || [];
  if (!records.length) return '';
  return `<h2>FROM BUILDS IN THIS PROJECT</h2>
    <div class="order">${records.map((t) => `<div class="order-row">
        <span class="order-name">${escape(t.name)}
          <span class="status">${count(t.records, 'record')}  ·  ${bytes(t.bytes)}</span></span>
        <button class="action" data-act="wz-teach" data-path="${escape(t.path)}" data-name="${escape(t.name)}"
                ${r.data.some((d) => d.id === t.path) ? 'disabled' : ''}>Add</button>
      </div>`).join('')}</div>
    <div class="hint" style="margin-top:.4rem">What a provider's model did here, as question-and-answer records with the files it wrote.</div>`;
}
actions['wz-teach'] = (e) => wizardTake('data',
  { source: 'local', id: e.dataset.path, path: e.dataset.path, label: `${e.dataset.name} (from builds)` }, 0);

function chosenList(list, slot) {
  if (!list.length) return '<div class="status">None.</div>';
  return `<div class="order">${list.map((a, i) => `<div class="order-row">
      <span class="order-name">${escape(a.label || a.id)}
        <span class="status">${a.source === 'local' ? 'this machine' : 'Huggingface'}</span></span>
      <button class="action" data-act="wz-remove" data-slot="${slot}" data-at="${i}">Remove</button>
    </div>`).join('')}</div>`;
}

function wizardStep(m) {
  const r = m.recipe;
  const fit = m.fit;
  switch (m.step) {
    case 0: return `
      <div class="field"><label for="wz-name">Name</label>
        <input id="wz-name" data-draft data-focus data-input="wz-field" data-field="name"
               value="${escape(r.name)}" placeholder="Kitchen Physicist">
        ${r.name ? `<div class="hint">saved as ${escape(r.id || slugOf(r.name))}</div>` : ''}</div>
      <div class="field"><label for="wz-purpose">Description</label>
        <textarea id="wz-purpose" rows="4" data-draft data-input="wz-field" data-field="purpose">${
          escape(r.purpose)}</textarea>
        <div class="hint">Used for routing.</div></div>`;
    case 1: return `
      <h2>FROM HUGGINGFACE</h2>${hubPicker(m, 'hub', 'model')}
      <h2 style="margin-top:1.4rem">OR A LOCAL FILE</h2>
      ${localPicker('base', 'model folder or file', 'Models', ['.gguf', '.safetensors', '.bin'], 'Use this')}
      <h2 style="margin-top:1.4rem">CHOSEN</h2>
      ${r.base.id ? `<div class="order-row"><span class="order-name">${escape(r.base.label || r.base.id)}
            <span class="status">${r.base.source === 'local' ? 'this machine' : 'Huggingface'}</span></span></div>
          <div class="field" style="margin-top:.7rem"><label for="wz-params">Parameters, in billions</label>
            <input id="wz-params" type="number" step="any" min="0" data-change="wz-params"
                   value="${r.parameters_b || ''}" placeholder="1.2">
            <div class="hint">${!r.parameters_b ? 'Unknown.'
              : fit && fit.methods[r.method].known
                ? `${r.method === 'lora' ? 'LoRA' : 'QLoRA'} needs ${bytes(fit.methods[r.method].needed)} of ${bytes(fit.methods[r.method].have)}.`
                : ''}</div></div>`
        : '<div class="status">None.</div>'}`;
    case 2: return `
      ${teachPicker(r)}
      <h2${state.teach && state.teach.length ? ' style="margin-top:1.4rem"' : ''}>FROM HUGGINGFACE</h2>${hubPicker(m, 'dataHub', 'dataset')}
      <h2 style="margin-top:1.4rem">OR LOCAL FILES</h2>
      ${localPicker('data', '.jsonl, .json, .csv or .parquet', 'Datasets',
                    ['.jsonl', '.json', '.csv', '.parquet', '.txt'], 'Add')}
      <h2 style="margin-top:1.4rem">CHOSEN</h2>${chosenList(r.data, 'data')}`;
    case 3: return `
      <h2>FROM HUGGINGFACE</h2>${hubPicker(m, 'toolHub', 'dataset')}
      <h2 style="margin-top:1.4rem">OR LOCAL FILES</h2>
      ${localPicker('tools', '.jsonl or .json', 'Datasets', ['.jsonl', '.json'], 'Add')}
      <h2 style="margin-top:1.4rem">CHOSEN</h2>${chosenList(r.tools, 'tools')}`;
    case 4: {
      const estimate = (id) => !fit ? '' : !fit.methods[id].known ? ''
        : `needs ${bytes(fit.methods[id].needed)} of ${bytes(fit.methods[id].have)}`;
      return `
      <h2>METHOD</h2>
      <div class="radios">${METHODS.map(([id, label, gloss]) => `<label class="radio${r.method === id ? ' on' : ''}">
          <input type="radio" name="wz-method" value="${id}" data-change="wz-field" data-field="method"${
            r.method === id ? ' checked' : ''}>
          <span><strong>${label}</strong><span class="hint">${gloss}</span>
            <span class="hint ${fit && fit.methods[id].known && !fit.methods[id].possible ? 'bad' : ''}">${
              estimate(id)}</span></span></label>`).join('')}</div>
      <h2 style="margin-top:1.4rem">QUANTIZATION</h2>
      <div class="radios tight">${QUANTIZATIONS.map((q) => `<label class="radio${r.quantization === q ? ' on' : ''}">
          <input type="radio" name="wz-quant" value="${q}" data-change="wz-field" data-field="quantization"${
            r.quantization === q ? ' checked' : ''}>
          <span><strong>${q}</strong><span class="hint">${
            fit && fit.sizes[q] ? 'about ' + bytes(fit.sizes[q]) : ''}</span></span></label>`).join('')}</div>
      <h2 style="margin-top:1.4rem">THE RUN</h2>
      <div class="field"><label for="wz-epochs">Epochs: ${r.epochs}</label>
        <input id="wz-epochs" type="range" min="1" max="10" step="1" value="${r.epochs}" data-follow
               data-input="wz-number" data-field="epochs"></div>
      <div class="field"><label for="wz-context">Context, in tokens: ${r.context}</label>
        <input id="wz-context" type="range" min="256" max="4096" step="128" value="${r.context}" data-follow
               data-input="wz-number" data-field="context"></div>
      <div class="field"><label for="wz-rate">Learning rate: ${rate(r.learning_rate)}</label>
        <input id="wz-rate" type="range" min="-6" max="-3" step="0.05" data-follow
               value="${Math.log10(r.learning_rate).toFixed(2)}" data-input="wz-rate">
        <div class="hint">Usually 1e-5 to 2e-4.</div></div>
      <h2 style="margin-top:1.4rem">FORMAT</h2>
      <div class="radios tight">${[['gguf', 'GGUF', ''],
                                   ['mlx', 'MLX', 'Apple silicon']].map(([id, label, gloss]) => `
        <label class="radio${r.format === id ? ' on' : ''}">
          <input type="radio" name="wz-format" value="${id}" data-change="wz-field" data-field="format"${
            r.format === id ? ' checked' : ''}>
          <span><strong>${label}</strong>${gloss ? `<span class="hint">${gloss}</span>` : ''}</span></label>`).join('')}</div>`;
    }
    default: {
      const missing = wizardMissing(r);
      const row = (label, value) => `<div><span>${label}</span>${value}</div>`;
      return `<h2>WHAT WILL RUN</h2>
      <div class="runtime-detail" style="border:0;padding:0">
        ${row('Expert', escape(r.name || '(unnamed)'))}
        ${row('Base model', r.base.id ? escape(r.base.label || r.base.id) : '<span class="bad">not chosen</span>')}
        ${row('Data', r.data.length ? r.data.map((d) => escape(d.label || d.id)).join(', ')
                                    : '<span class="bad">none</span>')}
        ${r.tools.length ? row('Tools', r.tools.map((d) => escape(d.label || d.id)).join(', ')) : ''}
        ${row('Method', r.method === 'lora' ? 'LoRA' : 'QLoRA')}
        ${row('Run', `${count(r.epochs, 'pass', 'passes')}  ·  ${r.context} tokens  ·  learning rate ${rate(r.learning_rate)}`)}
        ${row('Comes out as', `${r.format === 'mlx' ? 'MLX' : 'GGUF'} at ${escape(r.quantization)}`)}
      </div>
      <h2 style="margin-top:1.4rem">STILL MISSING</h2>
      <p class="lede${missing.length ? ' bad' : ''}">${missing.length ? missing.map(escape).join(', ') : 'nothing'}</p>`;
    }
  }
}

/// What a recipe still needs before it can run, said the way the engine
/// will say it once it is saved.
function wizardMissing(r) {
  const missing = [];
  if (!r.name.trim()) missing.push('a name');
  if (!r.base.id) missing.push('a base model');
  if (!r.data.length) missing.push('data');
  return missing;
}

modals.wizard = (m) => {
  const r = m.recipe;
  const last = m.step === WIZARD_STEPS.length - 1;
  const missing = wizardMissing(r);
  const blurb = WIZARD_STEPS[m.step][1];
  return `<div class="modal wide">
    <div class="head">
      <div class="chips left">${WIZARD_STEPS.map(([name], i) =>
        `<button class="chip${i === m.step ? ' on' : i < m.step ? ' done' : ''}" data-act="wz-step"
                 data-step="${i}">${i + 1}  ${name}</button>`).join('')}</div>
      ${blurb ? `<div class="status" style="margin-top:.8rem">${blurb}</div>` : ''}</div>
    <div class="body-pad wizard-body">${wizardStep(m)}
      ${m.error ? `<div class="bad" style="margin-top:.8rem">${escape(m.error)}</div>` : ''}</div>
    <div class="feet">
      <button class="action" data-act="wz-save">Save and close</button>
      <button class="action" data-act="modal-close">Discard</button>
      <span class="spacer"></span>
      ${last && missing.length ? `<span class="status">Still needs ${missing.map(escape).join(', ')}.</span>` : ''}
      <button class="action" data-act="wz-step" data-step="${m.step - 1}" ${m.step === 0 ? 'disabled' : ''}>Back</button>
      ${last ? `<button class="action toggle" aria-pressed="true" data-act="wz-train" ${
                  missing.length ? 'disabled' : ''}>Start training</button>`
             : `<button class="action" data-act="wz-step" data-step="${m.step + 1}">Next</button>`}
    </div></div>`;
};

function openWizard(recipe, step) {
  const blankHub = () => ({ query: '', items: [], searching: false, asked: false, error: '' });
  openModal({ kind: 'wizard', sticky: true, step: step || 0,
              recipe: JSON.parse(JSON.stringify(recipe)),
              hub: blankHub(), dataHub: blankHub(), toolHub: blankHub(), fit: null });
  refit();
  // What builds recorded here, for the Data step. Asked each time: a build
  // may have finished since the wizard was last open.
  if ((state.snapshot.project || {}).open) {
    call('teach.list').then((got) => { state.teach = got.experts || []; render(); }).catch(() => {});
  } else {
    state.teach = [];
  }
}

/// Ask what the recipe being edited would need, whenever its size changes.
function refit() {
  const m = state.modal;
  if (!m || m.kind !== 'wizard') return;
  call('lab.fit', { parameters_b: m.recipe.parameters_b || 0 })
    .then((fit) => { m.fit = fit; render(); }).catch(() => {});
}

actions['recipe-new']  = () => openWizard(blankRecipe(), 0);
actions['recipe-edit'] = (e) => {
  const r = recipesList().find((x) => x.id === e.dataset.id);
  if (r) openWizard(r, Number(e.dataset.step || 0));
};
actions['wz-step']  = (e) => {
  state.modal.step = Math.max(0, Math.min(WIZARD_STEPS.length - 1, Number(e.dataset.step)));
  render();
};
actions['wz-field'] = (e) => {
  state.modal.recipe[e.dataset.field] = e.value;
  // Redrawn either way. A choice changes what is highlighted, and typing a
  // name changes the line under it that says what it will be saved as; the
  // box itself is a draft, which a redraw leaves exactly as it is.
  render();
};
actions['wz-number'] = (e) => { state.modal.recipe[e.dataset.field] = Number(e.value); render(); };
actions['wz-rate']   = (e) => { state.modal.recipe.learning_rate = 10 ** Number(e.value); render(); };
actions['wz-params'] = (e) => { state.modal.recipe.parameters_b = Number(e.value) || 0; refit(); render(); };
actions['wz-query']  = (e) => { state.modal[e.dataset.slot].query = e.value; };
actions['wz-search'] = (form) => {
  const m = state.modal;
  const hub = m[form.dataset.slot];
  if (!hub.query.trim()) return;
  hub.searching = true; hub.error = ''; render();
  call('hub.search', { query: hub.query.trim(), kind: form.dataset.hub })
    .then((found) => { hub.items = found.items; })
    .catch((error) => { hub.items = []; hub.error = error.message; })
    .finally(() => { hub.searching = false; hub.asked = true; render(); });
};
/// Where a wizard slot puts what was chosen: the base is one thing, the
/// other two are lists.
function wizardTake(slot, asset, parameters) {
  const r = state.modal.recipe;
  if (slot === 'hub' || slot === 'base') {
    r.base = asset;
    r.parameters_b = parameters || 0;
    refit();
  } else {
    const list = slot === 'dataHub' || slot === 'data' ? r.data : r.tools;
    if (!list.some((a) => a.id === asset.id)) list.push(asset);
  }
  render();
}
actions['wz-use'] = (e) => wizardTake(e.dataset.slot,
  { source: 'hub', id: e.dataset.id, label: e.dataset.id }, Number(e.dataset.params));
actions['wz-local'] = (e) => {
  const box = document.getElementById('wz-local-' + e.dataset.slot);
  const path = box ? box.value.trim() : '';
  if (!path) return;
  wizardTake(e.dataset.slot, { source: 'local', id: path, path, label: fileName(path) }, 0);
  box.value = '';
};
actions['wz-browse'] = async (e) => {
  const modal = state.modal;
  const slot = e.dataset.slot;
  const path = await pickPath({ title: 'Choose a file', filter: e.dataset.filter,
                                extensions: e.dataset.ext.split(',') });
  if (!path || state.modal !== modal) return;
  wizardTake(slot, { source: 'local', id: path, path, label: fileName(path) }, 0);
};
actions['wz-remove'] = (e) => {
  const r = state.modal.recipe;
  (e.dataset.slot === 'data' ? r.data : r.tools).splice(Number(e.dataset.at), 1);
  render();
};
async function wizardSave() {
  const saved = await call('lab.save', state.modal.recipe);
  await reloadRecipes();
  return saved;
}
actions['wz-save'] = () => guard(async () => {
  if (!state.modal.recipe.name.trim()) throw new Error('It needs a name before it can be saved.');
  const saved = await wizardSave();
  state.modal.sticky = false;
  closeModal();
  state.open.recipe = saved.id;
});
actions['wz-train'] = () => guard(async () => {
  const saved = await wizardSave();
  await call('lab.train', { id: saved.id });
  state.run = await call('lab.run');
  await reloadRecipes();
  closeModal();
  state.open.recipe = saved.id;
});

// --- trying one before keeping it ---------------------------------------------------------------

modals.tester = (m) => {
  const turns = (state.snapshot.turns || []);
  const talk = turns.length > m.from ? turnsView(turns, m.from)
    : '';
  return `<div class="modal wide">
    <div class="head"><strong>Testing ${escape(m.name)}</strong>
      <div class="status">${escape(m.file)}</div></div>
    <div class="body-pad">
      <div class="listing" id="test-talk">${talk}</div>
      <form class="row" data-submit="test-send" style="margin-top:.8rem">
        <input id="test-prompt" data-draft data-focus placeholder="Ask it something"
               autocomplete="off" style="flex:1">
        <button class="action" ${state.snapshot.busy ? 'disabled' : ''}>Send</button></form>
      ${m.error ? `<div class="bad" style="margin-top:.6rem">${escape(m.error)}</div>` : ''}
    </div>
    <div class="feet">
      <button class="action" data-act="test-keep">Finish</button>
      <button class="action" data-act="test-edit">Edit</button>
      <button class="action" data-act="modal-close">Close</button>
      <span class="status">Finish adds it to the roster.</span>
    </div></div>`;
};

actions['recipe-test'] = (e) => guard(async () => {
  const r = recipesList().find((x) => x.id === e.dataset.id);
  if (!r) return;
  const seat = await call('lab.test', { id: r.id });
  openModal({ kind: 'tester', sticky: true, id: r.id, name: r.name,
              file: r.trained_display || r.trained_path,
              seat: seat.seat, from: seat.from,
              // However the window is closed, the seat goes with it.
              onClose: () => { call('lab.test.stop').catch(() => {}); } });
});
actions['test-send'] = (form) => {
  const box = form.querySelector('input');
  const text = box.value.trim();
  if (!text || state.snapshot.busy) return;
  box.value = '';
  guard(() => call('submit', { prompt: text, expert: state.modal.seat }));
};
actions['test-keep'] = () => guard(async () => {
  const m = state.modal;
  await call('lab.keep', { id: m.id });
  m.onClose = null;          // keeping it already took the seat away
  closeModal();
  await reloadRecipes();
  await reloadConfig();
});
actions['test-edit'] = () => {
  const id = state.modal.id;
  closeModal();
  const r = recipesList().find((x) => x.id === id);
  if (r) openWizard(r, 4);
};

/// Keep the test window's own transcript at its bottom, as Chat's is.
afterDraw.push(() => keepAtBottom('test-talk'));
