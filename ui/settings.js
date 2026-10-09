// SPDX-License-Identifier: MIT
//
// Settings.
//
// Every control here applies the moment it is changed. There are no Save
// buttons: a setting that has been moved and not yet saved is a state where
// somebody leaves the page believing they did something, and the only way to
// never be in it is to not have it.

const SETTINGS_PAGES = [
  ['general', 'General'], ['experts', 'Experts'], ['providers', 'Providers'],
  ['generation', 'Generation'], ['hardware', 'Hardware'], ['runtimes', 'Runtimes'],
  ['training', 'Training'], ['tools', 'Tools'], ['build', 'Build'], ['about', 'About'],
];

// --- one setting -------------------------------------------------------------------

/// A control bound to one path in the configuration.
///
/// `kind` is how it is drawn and how its value is read back: bool, int,
/// float, string, text (several lines), secret, choice (with `extra` as the
/// list of [value, label] pairs) or slider (with `extra` as {min, max, step}).
function setting(path, label, kind, hint, extra) {
  const value = at(state.config, path);
  const id = 'f-' + path.replace(/\./g, '-');
  const note = hint ? `<div class="hint">${hint}</div>` : '';
  const bind = { id, 'data-path': path, 'data-change': 'set' };

  if (kind === 'bool') {
    return `<div class="field"><label class="check"><input type="checkbox"${
      attrs({ ...bind, checked: !!value, disabled: extra && extra.disabled })}> ${label}</label>${note}</div>`;
  }
  if (kind === 'choice') {
    return `<div class="field"><label for="${id}">${label}</label><select${attrs(bind)}>${
      extra.map(([v, text]) => `<option value="${escape(v)}"${v === value ? ' selected' : ''}>${
        escape(text)}</option>`).join('')}</select>${note}</div>`;
  }
  if (kind === 'slider') {
    // A slider for getting close and a box for getting it exact, both the
    // same setting. Dragging redraws the number as it moves and saves when
    // the dragging stops.
    const numeric = Number.isInteger(extra.step) ? 'int' : 'float';
    const shared = { 'data-path': path, 'data-kind': numeric, min: extra.min, max: extra.max,
                     step: extra.step, value: value == null ? '' : value };
    return `<div class="field"><label for="${id}">${label}</label><div class="slider-row">
        <input type="range"${attrs({ ...shared, id, 'data-input': 'slide', 'data-follow': true })}>
        <input type="number"${attrs({ ...shared, 'data-change': 'set', 'aria-label': label })}>
      </div>${note}</div>`;
  }
  if (kind === 'text') {
    return `<div class="field"><label for="${id}">${label}</label><textarea rows="${
      (extra && extra.rows) || 5}"${attrs(bind)}>${escape(value || '')}</textarea>${note}</div>`;
  }
  const type = kind === 'int' || kind === 'float' ? 'number' : kind === 'secret' ? 'password' : 'text';
  return `<div class="field"><label for="${id}">${label}</label><input${
    attrs({ ...bind, type, 'data-kind': kind, value: value == null ? '' : value,
            step: kind === 'float' ? 'any' : null, autocomplete: 'off', spellcheck: 'false',
            placeholder: extra && extra.placeholder })}>${note}</div>`;
}

actions.set = (element) => configure(patchOf(element.dataset.path, valueOf(element)));

/// A slider being dragged: shown at once, saved a moment after it stops.
let slideTimer = null;
actions.slide = (element) => {
  const path = element.dataset.path;
  const value = valueOf(element);
  const keys = path.split('.');
  const leaf = keys.pop();
  const node = keys.reduce((n, key) => n[key], state.config);
  node[leaf] = value;
  render();
  clearTimeout(slideTimer);
  slideTimer = setTimeout(() => configure(patchOf(path, value)), 250);
};

/// A long install, as it goes: what it is doing, how far along, and -- when
/// it failed -- enough of the log to act on without opening the file.
function progressView(p, title, cancel, dismiss) {
  if (!p || p.phase === 'idle') return '';
  const failed = p.phase === 'failed';
  return `<div class="install">
      <div class="row"><strong>${escape(title)}</strong>
        <span class="${failed ? 'bad' : 'status'}">${escape(p.label || p.phase)}</span>
        ${p.running ? `<button class="action" data-act="${cancel}">Stop</button>`
                    : `<button class="action" data-act="${dismiss}">Dismiss</button>`}</div>
      ${p.running ? `<div class="bar"><span style="width:${Math.round((p.percent || 0) * 100)}%"></span></div>
        <div class="status">${escape(p.step || '')}</div>` : ''}
      ${failed ? `<div class="bad" style="margin-top:.6rem">${escape(p.error)}</div>` : ''}
      ${(failed || p.running) && p.log && p.log.length
        ? `<details class="work"${failed ? ' open' : ''}><summary>the log</summary>
             <pre class="install-log">${escape(p.log.join('\n'))}</pre>
             <div class="status">${escape(p.log_file)}</div></details>` : ''}
    </div>`;
}

// --- General -------------------------------------------------------------------------

function pageGeneral(c) {
  const models = state.models;
  const here = models ? models.models.length : null;
  return `<h1>General</h1>
    <h2>MODELS</h2>
    <div class="field"><label for="models-dir">Models folder</label>
      <div class="row">
        <input id="models-dir" data-change="models-dir" spellcheck="false"
               value="${escape(models ? models.directory : (c.models_dir || ''))}">
        <button class="action" data-act="models-browse">Browse</button>
      </div>
      <div class="hint">${here === null ? 'Looking...' : `${count(here, 'model')}, GGUF or MLX`}</div>
      <div class="row" style="margin-top:.6rem">
        <button class="action" data-act="models-rescan">Rescan</button>
        <button class="action" data-act="models-reset">Reset to default</button></div></div>

    <h2 style="margin-top:1.8rem">DELEGATOR</h2>
    <div class="field"><label for="router-model">Delegator model</label>
      ${modelSelect(c.router ? c.router.model : '', '',
          { id: 'router-model', 'data-change': 'router-model' }, true)}
      <div class="hint">Picks the expert. Local only.</div></div>
    ${setting('routing.keep_delegator_loaded', 'Keep loaded between prompts', 'bool', 'Uses more memory.')}
    ${setting('routing.min_confidence', 'Confidence floor', 'slider',
      'Below this, the default expert answers.', { min: 0, max: 1, step: 0.01 })}`;
}

actions['router-model'] = (e) => configure({ router: { model: splitModel(e.value).model } });
async function setModelsDir(path) {
  await configure({ models_dir: path });
  need('models', 'models', true);
}
actions['models-dir'] = (e) => setModelsDir(e.value.trim());
actions['models-browse'] = async () => {
  const path = await pickPath({ folder: true, title: 'Choose the models directory',
                                start: state.models ? state.models.directory : '' });
  if (path) await setModelsDir(path);
};
actions['models-rescan'] = () => need('models', 'models', true);
actions['models-reset'] = () => setModelsDir('');

// --- Experts --------------------------------------------------------------------------

function pageExperts(c) {
  const roster = state.snapshot.experts || [];
  const rows = roster.map((e) => {
    const seat = (c.experts || []).find((x) => x.id === e.id) || {};
    return `<div class="card">
        <div class="title"><strong>${escape(e.name)}</strong><span class="tag">[${escape(e.tag)}]</span>
          ${e.default ? '<span class="tag ok">default</span>' : ''}
          ${e.phase === 'missing' ? '<span class="tag bad">missing</span>' : ''}
          ${e.provider ? `<span class="tag">${ICONS.cloud} ${escape(e.provider)}</span>` : ''}
          <button class="link right" data-act="expert-remove" data-id="${escape(e.id)}"
                  data-name="${escape(e.name)}"
                  title="Remove from the roster">Eject</button></div>
        <div class="hint" style="margin:.2rem 0 .6rem">${escape(e.blurb)}</div>
        ${modelSelect(seat.model || '', seat.provider || '',
            { 'data-change': 'expert-model', 'data-id': e.id, 'aria-label': `Model for ${e.name}` })}
      </div>`;
  }).join('') || '<p class="lede">No experts yet.</p>';

  const folder = state.models ? state.models.display : (c.models_dir || '');
  return `<h1>Experts</h1>
    <div class="row" style="margin-bottom:.6rem">
      <button class="action" data-act="new-expert">New expert</button>
      <button class="action" data-act="models-rescan">Rescan models</button>
      <button class="link" data-act="view" data-view="create">Fine-tune one</button></div>
    <p class="lede" style="margin-bottom:1.2rem"><code>${escape(folder)}</code>
      <button class="link" data-act="models-browse">Change</button></p>
    ${rows}
    <h2 style="margin-top:1.8rem">DEFAULT EXPERT</h2>
    <div class="field">
      <select data-change="default-expert" aria-label="Default expert">
        <option value=""${!at(c, 'routing.default_expert') ? ' selected' : ''}>(none)</option>
        ${roster.map((e) => `<option value="${escape(e.id)}"${
          at(c, 'routing.default_expert') === e.id ? ' selected' : ''}>${escape(e.name)}</option>`).join('')}
      </select>
      <div class="hint">Answers when routing is unsure.</div></div>`;
}

const reloadConfig = async () => { state.config = await call('config'); };
actions['expert-model'] = (e) => guard(async () => {
  await call('expert.set', { id: e.dataset.id, ...splitModel(e.value) });
  await reloadConfig();
});
actions['expert-remove'] = async (e) => {
  const sure = await confirmIt({ title: `Eject ${e.dataset.name}?`,
    body: 'The model file stays.',
    yes: 'Eject', no: 'Keep' });
  if (sure) await guard(async () => { await call('expert.remove', { id: e.dataset.id }); await reloadConfig(); });
};
actions['default-expert'] = (e) => configure({ routing: { default_expert: e.value } });

// --- Providers ---------------------------------------------------------------------------

/// Where a provider's key comes from, in words. Never the key.
function keyLine(key) {
  if (key.source === 'typed') return 'key saved';
  if (key.source === 'variable' || key.source === 'convention') {
    return `$${escape(key.variable)}${key.present === false ? ' (not set)' : ''}`;
  }
  return 'no key';
}

function pageProviders() {
  const data = state.providers;
  if (!data) return '<h1>Providers</h1><p class="lede">Reading...</p>';
  const cards = data.providers.map((p) => `<div class="card">
      <div class="title"><strong>${escape(p.name)}</strong>
        <span class="tag">${p.kind === 'anthropic' ? 'Anthropic API' : 'OpenAI-compatible'}</span>
        ${p.on_your_network ? '<span class="tag">on your network</span>' : ''}
        <button class="link right" data-act="provider-edit" data-id="${escape(p.id)}">Edit</button>
        <button class="link" data-act="provider-remove" data-id="${escape(p.id)}"
                data-name="${escape(p.name)}">Remove</button></div>
      <div class="hint">${escape(p.endpoint)}</div>
      <div class="hint ${p.key.source === 'none' && p.kind === 'anthropic' ? 'bad' : ''}">${keyLine(p.key)}</div>
      <div class="hint">${p.models.length ? escape(p.models.join(', ')) : 'no model'}${
        p.seats.length ? `  ·  ${escape(p.seats.join(', '))}` : ''}</div>
    </div>`).join('');

  return `<h1>Providers</h1>
    <div class="row" style="margin-bottom:1.2rem">
      <button class="action" data-act="provider-new">Add a provider</button></div>
    ${cards}`;
}

/// The address a template stands for, written out. Anthropic's is the one the
/// API defaults to, which the list leaves empty.
const templateAddress = (k) => k.base_url || (k.kind === 'anthropic' ? 'https://api.anthropic.com' : '');

/// Add a provider, or change one: where it is, the key, and the model.
///
/// Nothing else is asked. Which of the two API shapes it speaks is worked out
/// from the address -- Anthropic's is Anthropic's, everything else speaks the
/// OpenAI one -- and what it is called comes from the template, or the
/// address when there was none. A template only fills in the address; it can
/// be ignored, and "Custom" is for anything not on the list.
modals.provider = (m) => {
  const known = state.providers ? state.providers.known : [];
  const preset = known.find((k) => k.name === m.preset);
  const listed = m.listed || [];
  return `<div class="modal">
    <div class="head"><strong>${m.id ? 'Edit ' + escape(m.name) : 'Add a provider'}</strong></div>
    <div class="body-pad">
      ${m.id ? '' : `<div class="field"><label for="pv-template">Template</label>
        <select id="pv-template" data-change="pv-template">
          <option value=""${m.preset ? '' : ' selected'}>Custom</option>
          ${known.map((k) => `<option value="${escape(k.name)}"${m.preset === k.name ? ' selected' : ''}>${
            escape(k.name)}</option>`).join('')}
        </select>
        ${preset && preset.note ? `<div class="hint">${escape(preset.note)}</div>` : ''}</div>`}
      <div class="field"><label for="pv-url">Address</label>
        <input id="pv-url" data-input="pv-field" data-field="base_url" data-draft spellcheck="false"
               value="${escape(m.base_url)}" placeholder="https://api.example.com/v1"
               ${m.id ? '' : 'data-focus'}></div>
      <div class="field"><label for="pv-key">API key</label>
        <input id="pv-key" type="password" data-input="pv-field" data-field="api_key" data-draft
               autocomplete="off" value="${escape(m.api_key)}"
               placeholder="${m.has_key ? 'saved' : preset && preset.key_variable
                 ? `key, env:NAME, or empty for $${escape(preset.key_variable)}` : 'key or env:NAME'}">
        <div class="hint">Stored in config.json.</div></div>
      <div class="field"><label for="pv-model">Model</label>
        <div class="row">
          <input id="pv-model" data-input="pv-field" data-field="model" data-draft spellcheck="false"
                 value="${escape(m.model)}">
          <button class="action" type="button" data-act="pv-list">${m.listing ? 'Asking...' : 'List'}</button>
        </div>
        ${listed.length ? `<select data-change="pv-pick" style="margin-top:.5rem" aria-label="Its models">
            <option value="">${count(listed.length, 'model')}</option>
            ${listed.map((name) => `<option value="${escape(name)}"${name === m.model ? ' selected' : ''}>${
              escape(name)}</option>`).join('')}</select>` : ''}
        ${m.note ? `<div class="hint">${escape(m.note)}</div>` : ''}</div>
      ${m.error ? `<div class="bad">${escape(m.error)}</div>` : ''}
    </div>
    <div class="feet">
      <button class="action" data-act="pv-save">${m.id ? 'Save' : 'Add provider'}</button>
      <button class="action" data-act="modal-close">Cancel</button>
    </div></div>`;
};

actions['provider-new'] = () => openModal({ kind: 'provider', id: '', name: '', preset: '',
  base_url: '', api_key: '', model: '', listed: [], has_key: false });
actions['provider-edit'] = (e) => {
  const p = state.providers.providers.find((x) => x.id === e.dataset.id);
  if (!p) return;
  openModal({ kind: 'provider', id: p.id, name: p.name, preset: '', base_url: p.endpoint,
              api_key: p.key.source === 'variable' ? 'env:' + p.key.variable : '',
              model: p.models[0] || '', listed: p.models, has_key: p.key.source === 'typed' });
};
actions['pv-template'] = (e) => {
  const m = state.modal;
  const k = (state.providers.known || []).find((x) => x.name === e.value);
  m.preset = k ? k.name : '';
  m.base_url = k ? templateAddress(k) : '';
  m.listed = [];
  m.note = '';
  // The fields are drafts, which a redraw leaves alone; this is the one time
  // the page means to overwrite what is in one.
  const field = document.getElementById('pv-url');
  if (field) field.value = m.base_url;
  render();
};
actions['pv-field'] = (e) => { state.modal[e.dataset.field] = e.value; };
actions['pv-pick'] = (e) => {
  if (!e.value) return;
  state.modal.model = e.value;
  const field = document.getElementById('pv-model');
  if (field) field.value = e.value;
};

/// What the form holds, as the request that saves or tests it.
///
/// The key is only sent when one was typed: an empty box means "the one on
/// file". The models are the one chosen, then any other this provider already
/// answers a seat with -- changing the model here must not take one away from
/// a seat that is using it.
function providerRequest(m) {
  const address = m.base_url.trim();
  const host = (address.match(/^[a-z]+:\/\/([^/:?#]+)/i) || [, ''])[1];
  const request = { base_url: address };
  if (m.id) {
    request.id = m.id;
  } else {
    request.name = m.preset || host || 'Provider';
  }
  if (m.api_key) request.api_key = m.api_key;
  const inUse = ((state.config && state.config.experts) || [])
    .filter((x) => m.id && x.provider === m.id).map((x) => x.model);
  request.models = [...new Set([m.model.trim(), ...inUse].filter(Boolean))];
  return request;
}
actions['pv-list'] = () => guard(async () => {
  const m = state.modal;
  m.listing = true; m.note = ''; render();
  try {
    const found = await call('provider.models', providerRequest(m));
    m.listed = found.models;
    m.note = '';
  } finally { m.listing = false; }
});
actions['pv-save'] = () => guard(async () => {
  const m = state.modal;
  if (!m.base_url.trim()) throw new Error('It needs an address.');
  const reply = await call('provider.save', providerRequest(m));
  closeModal();
  if (reply.warning) state.error = reply.warning;
  state.providers = await call('providers');
  await reloadConfig();
});
actions['provider-remove'] = async (e) => {
  const sure = await confirmIt({ title: `Remove ${e.dataset.name}?`,
    body: 'Its key is forgotten.', yes: 'Remove', no: 'Keep' });
  if (sure) await guard(async () => {
    await call('provider.remove', { id: e.dataset.id });
    state.providers = await call('providers');
    await reloadConfig();
  });
};

// --- Generation -----------------------------------------------------------------------------

function pageGeneration() {
  return `<h1>Generation</h1>
    <h2>SAMPLING</h2>
    ${setting('defaults.temperature', 'Temperature', 'slider', 'Higher is more random.',
      { min: 0, max: 2, step: 0.01 })}
    ${setting('defaults.top_p', 'Top P', 'slider', '', { min: 0, max: 1, step: 0.01 })}
    ${setting('defaults.top_k', 'Top K', 'slider', '0 is no limit.', { min: 0, max: 200, step: 1 })}
    ${setting('defaults.min_p', 'Min P', 'slider', '', { min: 0, max: 1, step: 0.01 })}
    ${setting('defaults.repeat_penalty', 'Repeat penalty', 'slider', '1 is off.', { min: 1, max: 2, step: 0.01 })}
    ${setting('defaults.repeat_last_n', 'Repeat window', 'int', 'Tokens.')}
    ${setting('defaults.max_tokens', 'Longest reply', 'int', 'Tokens. -1 is no limit.')}

    <h2 style="margin-top:1.8rem">PROMPTING</h2>
    ${setting('system_prompt', 'System prompt', 'text', '')}

    <h2 style="margin-top:1.8rem">CONTEXT</h2>
    ${setting('tools.overflow', 'When it is full', 'choice', '',
      [['rolling', 'Drop the oldest'], ['middle', 'Drop the middle'], ['stop', 'Stop']])}

    <h2 style="margin-top:1.8rem">LOADING</h2>
    ${setting('defaults.n_ctx', 'Context', 'int', 'Tokens.')}
    ${setting('defaults.n_gpu_layers', 'GPU layers', 'int', '-1 is all, 0 is none.')}
    ${setting('defaults.split_mode', 'Split', 'choice', '',
      [['layer', 'By layer'], ['row', 'By row'], ['none', 'No split']])}
    ${setting('defaults.n_batch', 'Batch', 'int', '')}
    ${setting('defaults.n_threads', 'Threads', 'int', '0 is every core.')}
    ${setting('defaults.flash_attn', 'Flash attention', 'bool', '')}`;
}

// --- Hardware ---------------------------------------------------------------------------------

const GPU_MODES = [
  ['auto', 'Automatic', 'llama.cpp decides.'],
  ['even', 'Even', 'By free memory.'],
  ['priority', 'Priority', 'Fill in order.'],
  ['single', 'One card', ''],
];

/// The order the cards are filled in, with any the config does not mention
/// appended: a card added since the order was set is still a card.
function gpuOrder() {
  const gpus = state.devices ? state.devices.gpus : [];
  const order = (at(state.config, 'gpu.priority') || []).filter((i) => i < gpus.length);
  for (let i = 0; i < gpus.length; i += 1) if (!order.includes(i)) order.push(i);
  return order;
}

function pageHardware(c) {
  const d = state.devices;
  if (!d) return '<h1>Hardware</h1><p class="lede">Looking...</p>';
  if (!d.gpus.length) {
    return `<h1>Hardware</h1><p class="lede">No GPUs found.</p>
      <button class="action" data-act="settings-page" data-page="runtimes">Runtimes</button>`;
  }
  const mode = at(c, 'gpu.mode') || 'auto';
  const order = gpuOrder();
  const unsplit = d.support.split;
  const cards = (mode === 'priority' ? order : d.gpus.map((_, i) => i)).map((index, place) => {
    const g = d.gpus[index];
    return `<div class="order-row">
        ${mode === 'priority' ? `<span class="order-n">${place + 1}</span>` : ''}
        <span class="order-name">${escape(g.name)} <span class="status">${escape(g.backend)}  ·  ${
          bytes(g.memory_free)} free of ${bytes(g.memory_total)}</span></span>
        ${mode === 'priority' ? `
          <button class="action" data-act="gpu-move" data-at="${place}" data-by="-1" ${
            place === 0 ? 'disabled' : ''} aria-label="Move up">&uarr;</button>
          <button class="action" data-act="gpu-move" data-at="${place}" data-by="1" ${
            place === order.length - 1 ? 'disabled' : ''} aria-label="Move down">&darr;</button>` : ''}
      </div>`;
  }).join('');

  return `<h1>Hardware</h1>
    <h2>SPLIT ACROSS CARDS</h2>
    ${unsplit ? `<p class="lede bad" style="margin-bottom:.8rem">${escape(unsplit)}</p>` : ''}
    <div class="radios">${GPU_MODES.map(([value, label, gloss]) => `
      <label class="radio${mode === value ? ' on' : ''}">
        <input type="radio" name="gpu-mode" value="${value}" data-change="set" data-path="gpu.mode"${
          mode === value ? ' checked' : ''}${unsplit && value !== 'auto' && value !== 'single' ? ' disabled' : ''}>
        <span><strong>${label}</strong>${gloss ? `<span class="hint">${gloss}</span>` : ''}</span></label>`).join('')}</div>

    <h2 style="margin-top:1.6rem">${mode === 'priority' ? 'ORDER' : 'CARDS'}</h2>
    <div class="order">${cards}</div>
    ${mode === 'single' ? `<div class="field" style="margin-top:1rem">
      <label for="main-gpu">The card</label>
      <select id="main-gpu" data-change="set" data-path="gpu.main_gpu" data-kind="int">${
        d.gpus.map((g, i) => `<option value="${i}"${at(c, 'gpu.main_gpu') === i ? ' selected' : ''}>${
          escape(g.name)}</option>`).join('')}</select></div>` : ''}

    <h2 style="margin-top:1.6rem">MEMORY</h2>
    ${setting('gpu.gpu_only', 'Keep every layer on the GPU', 'bool',
      d.support.gpu_only ? `<span class="bad">${escape(d.support.gpu_only)}</span>` : 'Refuse a partial load.',
      { disabled: !!d.support.gpu_only })}
    ${setting('gpu.vram_only', 'Use dedicated VRAM only', 'bool',
      d.support.vram_only ? `<span class="bad">${escape(d.support.vram_only)}</span>` : 'No spilling into system RAM.',
      { disabled: !!d.support.vram_only })}`;
}

actions['gpu-move'] = (e) => {
  const order = gpuOrder();
  const from = Number(e.dataset.at);
  const to = from + Number(e.dataset.by);
  if (to < 0 || to >= order.length) return;
  [order[from], order[to]] = [order[to], order[from]];
  configure({ gpu: { priority: order } });
};

// --- Runtimes ------------------------------------------------------------------------------------

function pageRuntimes() {
  const data = state.runtimes;
  if (!data) return '<h1>Runtimes</h1><p class="lede">Looking...</p>';
  if (!data.loadable) {
    return '<h1>Runtimes</h1><p class="lede">Built into this build.</p>';
  }
  const building = state.build && state.build.running;
  const cards = data.runtimes.map((r) => {
    const facts = [];
    if (r.installed) {
      facts.push(r.active ? count(r.devices, 'device') : 'no devices');
      facts.push(bytes(r.bytes));
      if (r.source) facts.push(r.source);
      if (r.built_at) facts.push(`built ${r.built_at}`);
    } else {
      facts.push('not installed');
      if (r.needs_tool) facts.push(`needs ${r.needs_tool}`);
    }
    const open = state.open.runtime === r.id;
    return `<div class="card">
        <div class="title"><strong>${escape(r.name)}</strong>
          <span class="tag ${r.active ? 'ok' : ''}">${r.active ? 'active' : r.installed ? 'idle' : ''}</span>
          ${r.stale ? '<span class="tag bad">built for another llama.cpp</span>' : ''}
          <button class="link right" data-act="runtime-open" data-id="${r.id}">${open ? 'Less' : 'More'}</button></div>
        <div class="hint">${escape(r.blurb)}</div>
        <div class="hint">${facts.map(escape).join('  ·  ')}</div>
        ${r.missing ? `<div class="hint bad">Missing ${escape(r.missing)}. Reinstall.</div>` : ''}
        ${!r.installed && !r.buildable && r.blocker ? `<div class="hint">${escape(r.blocker)}</div>` : ''}
        ${open ? `<div class="runtime-detail">
            <div><span>llama.cpp it was built for</span>${escape(r.llama_tag || 'unknown')}</div>
            <div><span>llama.cpp this build needs</span>${escape(r.needs_tag)}</div>
            <div><span>${count(r.modules.length, 'module')}</span>${
              r.modules.map((m) => escape(m.name) + (m.preferred && r.modules.length > 1 ? ' (in use)' : '')).join(', ')
                || 'none'}</div></div>` : ''}
        <div class="row" style="margin-top:.7rem">
          <button class="action" data-act="runtime-build" data-id="${r.id}" ${building ? 'disabled' : ''}>${
            r.installed ? 'Reinstall' : 'Install'}</button>
          ${r.installed ? `<button class="action" data-act="runtime-remove" data-id="${r.id}"
              data-name="${escape(r.name)}" ${building ? 'disabled' : ''}>Remove</button>` : ''}</div>
      </div>`;
  }).join('');
  return `<h1>Runtimes</h1>
    ${progressView(state.build, 'Installing ' + ((state.build && state.build.backend) || ''),
                   'runtime-cancel', 'runtime-dismiss')}
    ${cards}
    <p class="lede" style="margin-top:1.2rem">${escape(data.directory)}  ·  ${bytes(data.bytes)}</p>`;
}

const refreshRuntimes = () => { need('runtimes', 'runtimes', true); need('devices', 'devices', true); };
actions['runtime-open'] = (e) => {
  state.open.runtime = state.open.runtime === e.dataset.id ? null : e.dataset.id; render();
};
actions['runtime-build'] = (e) => guard(async () => {
  await call('runtime.build', { backend: e.dataset.id });
  state.build = await call('runtime.progress');
});
actions['runtime-cancel'] = () => guard(() => call('runtime.cancel'));
actions['runtime-dismiss'] = () => guard(async () => { await call('runtime.dismiss'); state.build = null; });
actions['runtime-remove'] = async (e) => {
  const sure = await confirmIt({ title: `Remove ${e.dataset.name}?`,
    body: 'Its files are deleted.', yes: 'Remove', no: 'Keep' });
  if (sure) await guard(async () => { await call('runtime.remove', { backend: e.dataset.id }); refreshRuntimes(); });
};

// --- Training ---------------------------------------------------------------------------------------

function pageTraining() {
  const t = state.trainer;
  const flavors = state.flavors || [];
  if (!t) return '<h1>Training</h1><p class="lede">Looking...</p>';
  const installing = state.install && state.install.running;
  const facts = !t.present ? '' : `<div class="runtime-detail" style="border:0;padding:0">
      <div><span>Built for</span>${escape(t.flavor)}</div>
      <div><span>Python</span>${escape(t.python || 'unknown')}</div>
      <div><span>PyTorch</span>${escape(t.torch || 'not importable')}</div>
      <div><span>Size</span>${bytes(t.bytes)}</div>
      ${t.installed_at ? `<div><span>Installed</span>${escape(t.installed_at)}</div>` : ''}
      ${t.usable_gpus.length ? `<div><span>Cards it can train on</span>${escape(t.usable_gpus.join(', '))}</div>` : ''}
      ${t.unusable_gpus.length ? `<div><span class="bad">Cards it cannot</span>${
        escape(t.unusable_gpus.join(', '))}</div>` : ''}
    </div>`;
  return `<h1>Training</h1>
    ${progressView(state.install, 'Installing the trainer', 'trainer-cancel', 'trainer-dismiss')}
    <div class="card">
      <div class="title"><strong>${t.ready ? 'Ready' : t.present ? 'Installed, but not working' : 'Not installed'}</strong>
        ${t.present ? `<span class="tag ${t.ready ? 'ok' : 'bad'}">${escape(t.note || '')}</span>` : ''}</div>
      ${facts}
      <div class="row" style="margin-top:.8rem; flex-wrap:wrap">
        ${flavors.map((f) => `<button class="action${f.suggested ? ' toggle' : ''}" data-act="trainer-install"
            data-flavor="${f.id}" aria-pressed="${f.suggested}" ${installing ? 'disabled' : ''}
            title="${escape(f.note)}\nAbout ${bytes(f.download)} to download, ${bytes(f.installed)} on disk.">${
            t.present && t.flavor === f.id ? (t.ready ? 'Reinstall' : 'Repair') : 'Install'} ${f.id}</button>`).join('')}
        ${t.present ? `<button class="action" data-act="trainer-remove" ${installing ? 'disabled' : ''}>Remove</button>` : ''}
      </div>
      <div class="hint" style="margin-top:.6rem">Highlighted: recommended.</div>
    </div>
    <details class="work"><summary>What goes in it</summary>
      ${flavors.map((f) => `<div class="act"><strong>${escape(f.id)}</strong>  ·  ${
        bytes(f.download)} to download, ${bytes(f.installed)} on disk<br>${
        f.steps.map(escape).join(', ')}</div>`).join('')}
      <div class="act">All in ${escape(t.directory)}. cuda includes NVIDIA's proprietary libraries.</div>
    </details>
    <p class="lede" style="margin-top:1.2rem"><code>crucible --install-trainer</code></p>`;
}

actions['trainer-install'] = (e) => guard(async () => {
  await call('trainer.install', { flavor: e.dataset.flavor });
  state.install = await call('trainer.progress');
});
actions['trainer-cancel'] = () => guard(() => call('trainer.cancel'));
actions['trainer-dismiss'] = () => guard(async () => { await call('trainer.dismiss'); state.install = null; });
actions['trainer-remove'] = async () => {
  const sure = await confirmIt({ title: 'Remove the training environment?',
    body: 'Downloaded base models go too. Your experts stay.',
    yes: 'Remove', no: 'Keep' });
  if (sure) await guard(async () => { await call('trainer.remove'); need('trainer', 'trainer', true); });
};

// --- Tools --------------------------------------------------------------------------------------------

function pageTools(c) {
  const project = state.snapshot.project || {};
  const searching = !!at(c, 'tools.web_search');
  const provider = at(c, 'tools.search_provider');
  const processes = state.processes || [];
  return `<h1>Tools</h1>
    <h2>PROJECT</h2>
    <p class="lede">${project.open ? escape(project.root) : 'None open.'}</p>
    <div class="hint" style="margin-bottom:1rem">In a trusted folder an expert can list, read and write files, run
      commands in any shell, run Python, use git${state.source && state.source.gh ? ' and gh' : ''}, and leave a
      server running.</div>
    ${setting('tools.workshop_timeout', 'Command timeout (seconds)', 'slider', '',
      { min: 5, max: 900, step: 5 })}
    ${processes.length ? `<div class="field"><label>Running now</label>${processes.map((p) => `<div class="order-row">
        <span class="order-name"><code>${escape(p.name)}</code> ${escape(p.command)}
          <span class="status">${p.running ? `${span(p.seconds)}` : `exited ${p.status}`}</span></span>
        <button class="action" data-act="process-stop" data-name="${escape(p.name)}">Stop</button></div>`).join('')}</div>` : ''}

    <h2 style="margin-top:1.8rem">THE WEB</h2>
    ${setting('tools.web_search', 'Web search and pages', 'bool', 'Lets experts search and read pages. Sends what they ask for out.')}
    ${searching ? `
      ${setting('tools.search_provider', 'Search with', 'choice', provider === 'duckduckgo' ? 'No key needed.' : '',
        [['duckduckgo', 'DuckDuckGo'], ['wikipedia', 'Wikipedia'], ['searxng', 'searxng'], ['brave', 'Brave']])}
      ${setting('tools.search_endpoint', 'Endpoint', 'string', provider === 'searxng' ? '' : 'searxng only.',
        { placeholder: 'http://localhost:8888' })}
      ${provider === 'brave' ? setting('tools.search_api_key', 'API key', 'secret', 'Stored in config.json.') : ''}
      ${setting('tools.search_results', 'Results', 'slider', '', { min: 1, max: 20, step: 1 })}
      ${setting('tools.search_timeout', 'Search timeout (seconds)', 'slider', '', { min: 2, max: 60, step: 1 })}
      ${setting('tools.search_rounds', 'Searches per prompt', 'slider', '', { min: 1, max: 10, step: 1 })}` : ''}

    <h2 style="margin-top:1.8rem">THIS COMPUTER</h2>
    ${setting('tools.computer_control', 'Let experts use this computer', 'bool',
      'Screenshots, the mouse and the keyboard, as you. Nothing bounds a click the way a folder bounds a file.')}
    ${at(c, 'tools.computer_control') ? `<div class="hint" style="margin-top:-.8rem;margin-bottom:1.4rem">
      On a Mac the first screenshot and the first click ask for Screen Recording and Accessibility under
      System Settings, Privacy &amp; Security. On Linux, xdotool and a screenshot tool are needed. With tesseract
      installed, a model that reads text only gets the words off each screenshot.</div>` : ''}`;
}

actions['process-stop'] = (button) => guard(async () => {
  await call('process.stop', { name: button.dataset.name });
  state.processes = (await call('processes')).processes;
});

// --- Build ---------------------------------------------------------------------------------------

function pageBuild(c) {
  const roster = state.snapshot.experts || [];
  const seated = roster.filter((e) => e.phase !== 'unconfigured');
  return `<h1>Build</h1>
    <h2>WHO PLANS IT</h2>
    <div class="field">
      <select data-change="set" data-path="build.architect" aria-label="Architect">
        <option value=""${!at(c, 'build.architect') ? ' selected' : ''}>The delegator picks</option>
        ${seated.map((e) => `<option value="${escape(e.id)}"${at(c, 'build.architect') === e.id ? ' selected' : ''}>${
          escape(e.name)}${e.provider ? '  ·  ' + escape(e.provider) : ''}</option>`).join('')}
      </select>
      <div class="hint">Writes the plan and the write-up. A frontier model does this best.</div></div>
    ${setting('build.confirm_plan', 'Show me the plan before it starts', 'bool', '')}

    <h2 style="margin-top:1.8rem">NEW AGENTS</h2>
    <div class="field"><label for="worker-model">Model for a seat a build makes</label>
      ${modelSelect(at(c, 'build.worker_model') || '', at(c, 'build.worker_provider') || '',
        { id: 'worker-model', 'data-change': 'worker-model' })}
      <div class="hint">When no expert fits a task, the build adds one on this model and keeps it. None: use the experts there are.</div></div>

    <h2 style="margin-top:1.8rem">THE RUN</h2>
    ${setting('build.auto_commit', 'Commit after each task', 'bool', 'Starts a repository when there is none.')}
    ${setting('build.rounds_per_task', 'Rounds per task', 'slider', 'Before the build moves on without it.',
      { min: 4, max: 200, step: 1 })}`;
}

actions['worker-model'] = (e) => {
  const chosen = splitModel(e.value);
  configure({ build: { worker_model: chosen.model, worker_provider: chosen.provider } });
};

// --- About -----------------------------------------------------------------------------------------------

function pageAbout() {
  const a = state.about;
  if (!a) return '<h1>About</h1><p class="lede">Reading...</p>';
  const u = a.update;
  const status = u.available
    ? `<p class="ok">Crucible ${escape(u.latest)} is available.</p>
       <p class="lede"><code>${escape(u.command)}</code><br>${escape(u.page)}</p>`
    : u.latest ? `<p class="lede">Up to date.</p>`
               : '<p class="lede">Not checked yet.</p>';
  const files = [['Configuration', a.files.config], ['Models', a.files.models],
                 ['Runtimes', a.files.runtimes], ['Python', a.files.python],
                 ['Projects and history', a.files.projects], ['Everything else', a.files.data],
                 ['Log', a.files.log], ['Crash reports', a.files.crashes]]
    .filter(([, path]) => path);
  return `<h1>About</h1><p class="lede">A local AI lab.</p>
    <div class="card"><div class="title"><strong>Crucible ${escape(a.version)}</strong></div>
      ${status}
      <div class="row"><button class="action" data-act="update-check">${
        state.open.checking ? 'Asking...' : 'Check now'}</button></div></div>
    ${setting('ui.check_updates', 'Check for new versions', 'bool', 'Daily, from GitHub.')}
    <h2 style="margin-top:1.8rem">WHAT LEAVES THIS MACHINE</h2>
    <p class="lede">The version check, web searches, first-start downloads, base models for
      training, and prompts to providers.</p>
    <h2 style="margin-top:1.8rem">FILES</h2>
    <div class="runtime-detail" style="border:0;padding:0">${files.map(([label, path]) =>
      `<div><span>${label}</span>${escape(path)}</div>`).join('')}</div>
    <h2 style="margin-top:1.8rem">TRUSTED FOLDERS</h2>
    ${a.trusted.length ? a.trusted.map((f) => `<div class="hint">${escape(f)}</div>`).join('')
                       : '<p class="lede">None.</p>'}`;
}

actions['update-check'] = () => guard(async () => {
  state.open.checking = true; render();
  try { await call('update.check'); state.about = await call('about'); }
  finally { state.open.checking = false; }
});

// --- the view ------------------------------------------------------------------------------------------------

views.settings = () => {
  const c = state.config;
  const page = !c ? '<p class="lede">Reading...</p>' : ({
    general: pageGeneral, experts: pageExperts, providers: pageProviders,
    generation: pageGeneration, hardware: pageHardware, runtimes: pageRuntimes,
    training: pageTraining, tools: pageTools, build: pageBuild, about: pageAbout,
  }[state.settingsPage] || pageGeneral)(c);
  return sideView() + `<div class="settings">
      <div class="settings-nav">${SETTINGS_PAGES.map(([id, label]) =>
        `<button data-act="settings-page" data-page="${id}"
                 aria-current="${state.settingsPage === id}">${label}</button>`).join('')}</div>
      <div class="settings-page">${page}</div>
    </div>`;
};

/// What Settings fetches when it opens. All of it at once, each on its own:
/// the pages share most of it, and none of it holds up the others.
entering.settings = () => {
  need('models', 'models');
  need('providers', 'providers');
  // Asked again every time, not kept from the start: the empty chat asks at
  // startup, before the engine has loaded any runtime, and that answer --
  // every runtime idle, driving nothing -- is wrong a second later.
  need('runtimes', 'runtimes', true);
  need('devices', 'devices');
  need('trainer', 'trainer');
  need('flavors', 'trainer.flavors');
  need('about', 'about', true);
  need('build', 'runtime.progress', true);
  need('install', 'trainer.progress', true);
  call('processes').then((got) => { state.processes = got.processes; render(); }).catch(() => {});
};

/// Follow a long install while one is running.
///
/// The installers poke the window as they go, and that arrives here as a
/// snapshot. So this asks how the job is doing each time one lands, and when
/// the job ends it rereads whatever the job changed.
let watching = false;
async function watchInstalls() {
  if (watching) return;
  const building = state.build && state.build.running;
  const installing = state.install && state.install.running;
  if (!building && !installing) return;
  watching = true;
  try {
    if (building) {
      state.build = await call('runtime.progress');
      if (!state.build.running) refreshRuntimes();
    }
    if (installing) {
      state.install = await call('trainer.progress');
      if (!state.install.running) need('trainer', 'trainer', true);
    }
  } catch (e) { /* asked again on the next snapshot */ }
  watching = false;
  render();
}
onSnapshot.push(watchInstalls);
