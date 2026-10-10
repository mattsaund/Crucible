// SPDX-License-Identifier: MIT
//
// The preview: a page of the project, shown, and a dashboard for nudging it.
//
// What is being built is shown as it is -- an HTML file in the project, with
// its stylesheets, scripts and pictures put inside it by the session (see
// tools/preview.hpp) and drawn in a frame. Pick an element in it and the
// dashboard shows what it is and the handful of things most worth changing
// by eye: spacing, size, type, color. Each change is applied to the frame
// at once, and when it looks right the whole set is handed to an expert as
// a prompt to make in the source, since a change the page forgets on reload
// is not a change.
//
// The frame is sandboxed with scripts allowed and nothing else, so the page
// runs but cannot reach this window; the two talk only through messages.
// The script below is put into the page for the picking and the live
// changes, and nothing it is sent is anything but a style value.

state.preview = { pages: null, page: '', url: '', html: '', inlined: [], skipped: [], picking: false,
                  picked: null, changes: [], error: '', loading: false, stale: false,
                  making: null, made: null };

/// What pages in the preview have saved, by project and page, for as long
/// as the window is open. See DATA_SCRIPT.
const previewData = {};

/// What a page of the project is given to keep its data with: the same
/// window.crucible.load() and save(data) the program it is packaged as
/// answers (see tools/app_runner.cpp), answered here by the window that
/// shows it. A frame with scripts and nothing else allowed has no storage of
/// its own, and an app that saved nothing would lose every figure typed into
/// it at the next reload of the preview.
///
/// Put at the very start of the page, before any of its own scripts run.
const DATA_SCRIPT = `
(function () {
  if (window.crucible) return;
  var waiting = {}, next = 0;
  function ask(what, data) {
    return new Promise(function (resolve) {
      var id = ++next;
      waiting[id] = resolve;
      parent.postMessage({ crucible: 'data', what: what, id: id, data: data }, '*');
    });
  }
  window.addEventListener('message', function (e) {
    var m = e.data || {};
    if (m.crucible === 'data-answer' && waiting[m.id]) { waiting[m.id](m.data); delete waiting[m.id]; }
  });
  window.crucible = {
    preview: true,
    load: function () { return ask('load'); },
    save: function (data) { return ask('save', data == null ? null : data); }
  };
})();
`;

/// What goes into the previewed page, as text: the picker and the applier.
///
/// Written as a string rather than a function, because it runs in another
/// document and is sent as source. Kept plain: no modern syntax that an
/// older webview inside a webview might refuse.
const TUNE_SCRIPT = `
(function () {
  var picking = false, hover = null, chosen = null, outline = null, set = [];
  function selectorOf(el) {
    if (el.id) return '#' + el.id;
    var parts = [];
    for (var node = el; node && node !== document.body && parts.length < 5; node = node.parentElement) {
      var part = node.tagName.toLowerCase();
      var cls = (node.className && typeof node.className === 'string')
        ? node.className.trim().split(/\\s+/).filter(function (c) { return c && !/^crucible/.test(c); }).slice(0, 2) : [];
      if (cls.length) part += '.' + cls.join('.');
      var same = 0, index = 0;
      var kids = node.parentElement ? node.parentElement.children : [];
      for (var i = 0; i < kids.length; i++) {
        if (kids[i].tagName === node.tagName) { same++; if (kids[i] === node) index = same; }
      }
      if (same > 1) part += ':nth-of-type(' + index + ')';
      parts.unshift(part);
      if (node.id) { parts[0] = '#' + node.id; break; }
    }
    return parts.join(' > ');
  }
  function styles(el) {
    var cs = getComputedStyle(el);
    var out = {};
    ['padding', 'margin', 'font-size', 'font-family', 'font-weight', 'line-height', 'color',
     'background-color', 'border-radius', 'text-align', 'gap', 'width', 'letter-spacing'].forEach(function (p) {
      out[p] = cs.getPropertyValue(p);
    });
    return out;
  }
  function mark(el) {
    if (hover && hover !== chosen) hover.style.outline = hover.__crucibleOutline || '';
    hover = el;
    if (el && el !== chosen) { el.__crucibleOutline = el.style.outline; el.style.outline = '2px dashed #FF8700'; }
  }
  function choose(el) {
    if (chosen) chosen.style.outline = chosen.__crucibleOutline || '';
    chosen = el;
    if (el) { el.__crucibleOutline = el.style.outline; el.style.outline = '2px solid #FF8700'; }
    var rect = el ? el.getBoundingClientRect() : null;
    parent.postMessage({ crucible: 'picked', selector: el ? selectorOf(el) : '', tag: el ? el.tagName.toLowerCase() : '',
      text: el ? (el.textContent || '').trim().slice(0, 80) : '', styles: el ? styles(el) : {},
      box: rect ? { w: Math.round(rect.width), h: Math.round(rect.height) } : null }, '*');
  }
  document.addEventListener('mouseover', function (e) { if (picking) mark(e.target); }, true);
  document.addEventListener('click', function (e) {
    if (!picking) return;
    e.preventDefault(); e.stopPropagation();
    choose(e.target);
  }, true);
  window.addEventListener('message', function (e) {
    var m = e.data || {};
    if (m.crucible === 'pick') { picking = !!m.on; if (!picking) mark(null); document.body.style.cursor = picking ? 'crosshair' : ''; }
    if (m.crucible === 'style' && m.selector) {
      try {
        var el = document.querySelector(m.selector);
        if (el) { el.style.setProperty(m.prop, m.value, 'important'); set.push([el, m.prop]); }
      } catch (err) { /* a selector the page cannot find */ }
    }
    if (m.crucible === 'reset') {
      set.forEach(function (pair) { pair[0].style.removeProperty(pair[1]); });
      set = [];
      choose(null);
    }
    if (m.crucible === 'clear') { choose(null); }
  });
  parent.postMessage({ crucible: 'ready', title: document.title || '' }, '*');
})();
`;

/// The properties the dashboard offers, and how each is edited.
const TUNE_PROPS = [
  ['padding', 'Padding', 'text'], ['margin', 'Margin', 'text'], ['gap', 'Gap', 'text'],
  ['font-size', 'Font size', 'text'], ['line-height', 'Line height', 'text'],
  ['font-family', 'Font', 'text'], ['font-weight', 'Weight', 'text'], ['letter-spacing', 'Tracking', 'text'],
  ['color', 'Color', 'color'], ['background-color', 'Background', 'color'],
  ['border-radius', 'Rounding', 'text'], ['text-align', 'Align', 'text'], ['width', 'Width', 'text'],
];

/// The page with the picker inside it, for the frame's srcdoc.
function tunedDocument(html) {
  // The closing tags are split, because this file is itself inside a script
  // element of the bundled page, and the HTML parser would end that
  // element at the first closing script tag it saw, whatever string it was in.
  const script = `<script>${TUNE_SCRIPT}<\/script>`;
  const data = `<script>${DATA_SCRIPT}<\/script>`;
  // The data calls first of all, so the page's own scripts find them; the
  // picker last, once there is a page to pick from.
  const head = /<head[^>]*>/i.exec(html);
  let out = head ? html.slice(0, head.index + head[0].length) + data + html.slice(head.index + head[0].length)
                 : data + html;
  const at = out.lastIndexOf('</body>');
  out = at >= 0 ? out.slice(0, at) + script + out.slice(at) : out + script;
  return out;
}

/// The key a page's preview data is remembered under, per project and page.
function previewDataKey() {
  return 'preview-data:' + ((state.snapshot.project || {}).root || '') + ':' + state.preview.page;
}

/// "rgb(255, 135, 0)" as "#ff8700", for a color input; anything else as is.
function hexOf(value) {
  const m = /^rgba?\((\d+),\s*(\d+),\s*(\d+)/.exec(value || '');
  if (!m) return /^#/.test(value || '') ? value : '#000000';
  return '#' + [m[1], m[2], m[3]].map((n) => Number(n).toString(16).padStart(2, '0')).join('');
}

function previewView() {
  const p = state.preview;
  const project = state.snapshot.project || {};
  if (!project.open) return '<div class="empty"><div class="empty-label">No project open.</div></div>';
  const pages = p.pages || [];
  // A file in the project, or an address: a server the build started with
  // START, or one running anyway. A page by address runs as itself and
  // cannot be picked at, since nothing of ours is inside it.
  const picker = `<div class="row preview-bar">
      <select data-change="preview-page" aria-label="Page to preview">
        <option value="">${pages.length ? 'Choose a page' : 'No HTML pages in the project'}</option>
        ${pages.map((page) => `<option value="${escape(page)}"${page === p.page && !p.url ? ' selected' : ''}>${escape(page)}</option>`).join('')}
      </select>
      <form class="row" data-submit="preview-url" style="flex:1;gap:.4rem">
        <input id="preview-url" data-draft value="${escape(p.url)}" placeholder="or an address: http://localhost:3000"
               aria-label="Address to preview" autocomplete="off" spellcheck="false">
        <button class="action" type="submit">Go</button></form>
      <button class="action" data-act="preview-reload" ${p.page || p.url ? '' : 'disabled'}>Reload</button>
      <button class="action toggle" data-act="preview-pick" aria-pressed="${p.picking}" ${p.html && !p.url ? '' : 'disabled'}
              title="${p.url ? 'A page by address cannot be picked at' : 'Pick an element to tune'}">${
        p.picking ? 'Picking' : 'Pick an element'}</button>
      <span class="status">${p.loading ? 'Reading...' : apart(!p.url && p.inlined.length ? `${count(p.inlined.length, 'file')} inlined` : '',
        !p.url && p.skipped.length ? `${count(p.skipped.length, 'file')} left out` : '')}</span>
      <span class="spacer"></span>
      <button class="action" data-act="app-make" ${p.html && !p.url && !p.making ? '' : 'disabled'}
              title="Make this page a program you open like any other">Make it an app</button>
    </div>${appStrip()}`;
  const frame = p.url
    ? `<iframe id="preview-frame" class="preview-frame" sandbox="allow-scripts allow-forms allow-modals allow-same-origin allow-popups"
         title="Preview of ${escape(p.url)}" src="${escape(p.url)}"></iframe>`
    : p.html
    ? `<iframe id="preview-frame" class="preview-frame" sandbox="allow-scripts allow-forms allow-modals"
         title="Preview of ${escape(p.page)}" srcdoc="${escape(tunedDocument(p.html))}"></iframe>`
    : `<div class="preview-empty status">${p.error ? escape(p.error) : 'Choose a page, or give an address.'}</div>`;
  return `<div class="preview">${picker}<div class="preview-body">${frame}${p.url ? '' : tuneView()}</div></div>`;
}

/// Under the bar: the name the app will have while it is being made, what
/// was made and how to open it, or that a build has changed the page since
/// it was shown.
function appStrip() {
  const p = state.preview;
  const where = (state.snapshot.platform || '') === 'windows' ? 'the Start menu'
              : (state.snapshot.platform || '') === 'linux' ? 'your applications menu' : 'Applications';
  if (p.making) {
    return `<form class="row app-strip" data-submit="app-package">
        <label for="app-name">Name</label>
        <input id="app-name" data-draft value="${escape(p.making.name)}" autocomplete="off" spellcheck="false"
               ${p.making.busy ? 'disabled' : ''}>
        <button class="action" type="submit" ${p.making.busy || !p.making.can ? 'disabled' : ''}>${
          p.making.busy ? 'Making it...' : 'Make it'}</button>
        <button class="action" type="button" data-act="app-cancel" ${p.making.busy ? 'disabled' : ''}>Cancel</button>
        <span class="status">${p.making.can ? `It goes in ${where}, and keeps its data between uses.`
          : 'This Crucible has no app runner beside it -- reinstall Crucible to make apps.'}</span>
      </form>`;
  }
  if (p.made) {
    return `<div class="row app-strip made">
        <span><strong>${escape(p.made.name)}</strong> is ready, ${escape(p.made.where)}.</span>
        <button class="action" data-act="app-open">Open ${escape(p.made.name)}</button>
      </div>`;
  }
  if (p.stale) {
    return `<div class="row app-strip">
        <span class="status">The build has changed the page since it was shown.</span>
        <button class="action" data-act="preview-reload">Show it now</button>
      </div>`;
  }
  return '';
}

/// The dashboard beside the frame: what was picked, and the dials.
function tuneView() {
  const p = state.preview;
  if (!p.html) return '';
  const e = p.picked;
  if (!e) {
    return `<div class="tune"><div class="caption">TUNE</div>
      <div class="status">${p.picking ? 'Click an element in the page.' : 'Pick an element to change its spacing, type and color.'}</div>
      ${p.changes.length ? tuneChanges() : ''}</div>`;
  }
  const rows = TUNE_PROPS.map(([prop, label, kind]) => {
    const current = (p.changes.find((c) => c.selector === e.selector && c.prop === prop) || {}).value
                  || (e.styles || {})[prop] || '';
    const id = 'tune-' + prop;
    if (kind === 'color') {
      return `<div class="tune-row"><label for="${id}">${label}</label>
        <input type="color" id="${id}" value="${escape(hexOf(current))}" data-input="tune-set" data-prop="${prop}" data-follow>
        <input class="tune-text" value="${escape(current)}" data-change="tune-set" data-prop="${prop}" aria-label="${label} value" data-draft></div>`;
    }
    return `<div class="tune-row"><label for="${id}">${label}</label>
      <input id="${id}" value="${escape(current)}" data-change="tune-set" data-prop="${prop}" data-draft spellcheck="false">
      ${['padding', 'margin', 'gap', 'font-size', 'border-radius', 'letter-spacing', 'line-height'].includes(prop)
        ? `<button class="icon" data-act="tune-nudge" data-prop="${prop}" data-by="-1" title="Less" aria-label="Less ${label}">&minus;</button>
           <button class="icon" data-act="tune-nudge" data-prop="${prop}" data-by="1" title="More" aria-label="More ${label}">+</button>` : ''}
    </div>`;
  }).join('');
  return `<div class="tune"><div class="caption">TUNE</div>
    <div class="tune-what"><strong>${escape(e.tag)}</strong> <span class="status">${escape(e.selector)}</span>
      ${e.text ? `<div class="hint">"${escape(e.text)}"</div>` : ''}
      ${e.box ? `<div class="hint">${e.box.w} × ${e.box.h} px</div>` : ''}</div>
    <div class="tune-rows">${rows}</div>
    ${tuneChanges()}</div>`;
}

/// The changes made so far, and the two things to do with them.
function tuneChanges() {
  const p = state.preview;
  if (!p.changes.length) return '';
  return `<div class="tune-changes"><div class="caption">CHANGES</div>
    <div class="tune-list">${p.changes.map((c) => `<div class="hint"><code>${escape(c.selector)}</code> ${escape(c.prop)}: ${escape(c.value)}</div>`).join('')}</div>
    <div class="row" style="margin-top:.6rem">
      <button class="action" data-act="tune-apply" ${state.snapshot.busy ? 'disabled' : ''}
              title="Hand these to an expert to make in the source">Apply to source</button>
      <button class="action" data-act="tune-reset">Discard</button></div></div>`;
}

/// The prompt an expert gets: every change, said once per element.
function tunePrompt() {
  const p = state.preview;
  const groups = {};
  for (const c of p.changes) {
    (groups[c.selector] = groups[c.selector] || []).push(`${c.prop}: ${c.value}`);
  }
  const lines = Object.entries(groups).map(([selector, props]) =>
    `- the element matching \`${selector}\` should have ${props.join('; ')}`);
  // Said as work to do in the file, not a question about it: a small model
  // asked only to "make these changes" answers with a snippet and stops.
  return `Edit the project so that ${p.page} shows these style changes. Read ${p.page} first, change the style where it lives (the stylesheet it uses, or the markup when the style is inline), and write the changed file back with everything else exactly as it was:\n${
    lines.join('\n')}\nUse the same units the file already uses where that is sensible. Do not just show the change -- make it in the file, and then say which file you changed.`;
}

// --- what a person can do here ------------------------------------------------------

function previewFrame() { return document.getElementById('preview-frame'); }
function tellFrame(message) {
  const frame = previewFrame();
  if (frame && frame.contentWindow) frame.contentWindow.postMessage(message, '*');
}

async function loadPreview(page) {
  const p = state.preview;
  p.page = page;
  p.html = '';
  p.picked = null;
  p.changes = [];
  p.picking = false;
  p.error = '';
  p.stale = false;
  if (!page) return render();
  p.loading = true;
  render();
  try {
    const got = await call('preview.page', { path: page });
    p.html = got.html;
    p.inlined = got.inlined || [];
    p.skipped = got.skipped || [];
  } catch (error) {
    p.error = error.message;
  }
  p.loading = false;
  render();
}

actions['preview-page']   = (select) => { state.preview.url = ''; loadPreview(select.value); };
actions['preview-reload'] = () => {
  const p = state.preview;
  if (p.url) {
    // The same address again is not a change to the attribute, so the frame
    // is reloaded by hand.
    const frame = previewFrame();
    if (frame) frame.src = p.url;
    return;
  }
  loadPreview(p.page);
};
actions['preview-url'] = (form) => {
  const p = state.preview;
  const url = form.querySelector('input').value.trim();
  if (!url) { p.url = ''; render(); return; }
  if (!/^https?:\/\//i.test(url)) { state.error = 'An address starts with http:// or https://'; render(); return; }
  p.url = url;
  p.html = '';
  p.picked = null;
  p.changes = [];
  p.picking = false;
  p.error = '';
  render();
};
actions['preview-pick'] = () => {
  const p = state.preview;
  p.picking = !p.picking;
  tellFrame({ crucible: 'pick', on: p.picking });
  render();
};

/// Record and apply one change.
function tuneSet(prop, value) {
  const p = state.preview;
  if (!p.picked || !value) return;
  const existing = p.changes.find((c) => c.selector === p.picked.selector && c.prop === prop);
  if (existing) existing.value = value; else p.changes.push({ selector: p.picked.selector, prop, value });
  tellFrame({ crucible: 'style', selector: p.picked.selector, prop, value });
  render();
}
actions['tune-set'] = (input) => tuneSet(input.dataset.prop, input.value.trim());

/// A notch up or down on a length: the first number in it, by a pixel or two.
actions['tune-nudge'] = (button) => {
  const p = state.preview;
  if (!p.picked) return;
  const prop = button.dataset.prop;
  const current = (p.changes.find((c) => c.selector === p.picked.selector && c.prop === prop) || {}).value
                || (p.picked.styles || {})[prop] || '0px';
  const step = Number(button.dataset.by) * (prop === 'line-height' && !/px/.test(current) ? 0.1 : prop === 'letter-spacing' ? 0.5 : 2);
  const m = /^(-?[\d.]+)(px|rem|em|%)?/.exec(current.trim());
  const base = m ? Number(m[1]) : 0;
  const unit = m && m[2] ? m[2] : (prop === 'line-height' && !/px/.test(current) ? '' : 'px');
  const next = Math.max(0, Math.round((base + step) * 100) / 100);
  // "8px 16px" is four sides; a nudge moves all of them together.
  const rest = current.trim().replace(/^-?[\d.]+(px|rem|em|%)?/, '').trim();
  tuneSet(prop, `${next}${unit}${rest && /^\d/.test(rest) ? ' ' + rest : ''}`);
};

actions['tune-reset'] = () => {
  const p = state.preview;
  p.changes = [];
  p.picked = null;
  tellFrame({ crucible: 'reset' });
  render();
};

/// The changes, made in the source. As a build of one task when nothing is
/// building -- the person stays where they are, looking at the page, and it
/// reloads with the change made -- and as a chat prompt when a build has the
/// box already.
actions['tune-apply'] = () => guard(async () => {
  const prompt = tunePrompt();
  if (!prompt) return;
  const building = state.snapshot.cook && state.snapshot.cook.running;
  if (building) {
    await call('submit', { prompt, expert: routeTarget() });
    state.follow = true;
  } else {
    await call('build.start', { directive: prompt, attachments: [], expert: routeTarget() });
  }
  state.preview.changes = [];
  tellFrame({ crucible: 'reset' });
  if (building) enter('chat');
  else render();
});

/// Make the page a program: first the name it will have, then the program.
actions['app-make'] = () => guard(async () => {
  const p = state.preview;
  const got = await call('app.name', { page: p.page || 'index.html' });
  p.making = { name: got.name || 'App', busy: false, can: got.can !== false };
  p.made = null;
  render();
  const box = document.getElementById('app-name');
  if (box) { box.focus(); box.select(); }
});
actions['app-cancel'] = () => { state.preview.making = null; render(); };
actions['app-package'] = (form) => guard(async () => {
  const p = state.preview;
  const name = form.querySelector('input').value.trim();
  if (!name || !p.making || p.making.busy) return;
  p.making.busy = true;
  render();
  try {
    p.made = await call('app.package', { page: p.page || 'index.html', name });
    p.making = null;
  } finally {
    if (p.making) p.making.busy = false;
    render();
  }
});
actions['app-open'] = () => guard(() => call('app.open', { launch: state.preview.made.launch }));

/// What the page says back: the element picked, or its data to keep.
function previewMessage(m) {
  if (m.crucible === 'data') {
    // Kept for as long as the window is open, and in this window's own
    // storage when it has some, under the project and the page.
    const key = previewDataKey();
    if (m.what === 'save') {
      previewData[key] = m.data;
      try { localStorage.setItem(key, JSON.stringify(m.data)); } catch (e) { /* no storage: memory will do */ }
      tellFrame({ crucible: 'data-answer', id: m.id, data: true });
    } else {
      let saved = key in previewData ? previewData[key] : null;
      if (saved === null) {
        try { saved = JSON.parse(localStorage.getItem(key) || 'null'); } catch (e) { saved = null; }
      }
      tellFrame({ crucible: 'data-answer', id: m.id, data: saved });
    }
    return;
  }
  if (m.crucible === 'picked') {
    state.preview.picked = m.selector ? { selector: m.selector, tag: m.tag, text: m.text, styles: m.styles, box: m.box } : null;
    // One pick ends the picking, the way a browser's inspector does, so the
    // page can be used again straight after; the button starts it over.
    if (state.preview.picking && m.selector) {
      state.preview.picking = false;
      tellFrame({ crucible: 'pick', on: false });
    }
    render();
  }
}
window.addEventListener('message', (event) => previewMessage(event.data || {}));

/// What the preview fetches when its pane opens: the pages there are.
function enterPreview() {
  if ((state.snapshot.project || {}).open) {
    call('preview.candidates').then((got) => {
      state.preview.pages = got.pages || [];
      if (!state.preview.page && state.preview.pages.length) loadPreview(state.preview.pages[0]);
      render();
    }).catch((error) => { state.preview.error = error.message; render(); });
  }
}
