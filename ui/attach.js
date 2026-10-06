// SPDX-License-Identifier: MIT
//
// Things attached to a prompt or a goal: files, pictures, a folder.
//
// The gray plus at the bottom left of the box opens a menu of two -- files
// or photos, or a folder -- and either one opens the platform's own file
// dialog. What comes back sits in the box as tiles above the text, where
// more can still be typed, until it is sent with it. The window reads none of
// it: the session says what each one is and reads it when the prompt goes,
// sized to whichever expert answers. The one thing done here is pictures,
// which are shrunk to what a provider takes before they are sent, because a
// canvas is the one image codec this program has.
//
// Chat and Cook each keep their own: a file attached to a question is not
// meant for the goal on the other tab.

const CLIP = {
  plus: `<svg viewBox="0 0 16 16" width="16" height="16" aria-hidden="true" fill="none" stroke="currentColor"
    stroke-width="1.5" stroke-linecap="round"><path d="M8 2.5v11M2.5 8h11"/></svg>`,
  file: `<svg viewBox="0 0 24 24" width="17" height="17" aria-hidden="true" fill="none" stroke="currentColor"
    stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"><path d="M21.4 11.1l-9.2 9.2a6 6 0 0 1-8.5-8.5l9.2-9.2a4 4 0 0 1 5.7 5.7l-9.2 9.2a2 2 0 0 1-2.8-2.8l8.5-8.5"/></svg>`,
  folder: `<svg viewBox="0 0 24 24" width="17" height="17" aria-hidden="true" fill="none" stroke="currentColor"
    stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"><path d="M3 6.5A1.5 1.5 0 0 1 4.5 5h4l2 2.5h9A1.5 1.5 0 0 1 21 9v9.5a1.5 1.5 0 0 1-1.5 1.5h-15A1.5 1.5 0 0 1 3 18.5z"/><path d="M12 11v6M9 14h6"/></svg>`,
  shut: `<svg viewBox="0 0 16 16" width="11" height="11" aria-hidden="true" fill="none" stroke="currentColor"
    stroke-width="1.8" stroke-linecap="round"><path d="M4 4l8 8M12 4l-8 8"/></svg>`,
  box: `<svg viewBox="0 0 24 24" width="26" height="26" aria-hidden="true" fill="none" stroke="currentColor"
    stroke-width="1.3" stroke-linejoin="round"><path d="M3 6.5A1.5 1.5 0 0 1 4.5 5h4l2 2.5h9A1.5 1.5 0 0 1 21 9v9.5a1.5 1.5 0 0 1-1.5 1.5h-15A1.5 1.5 0 0 1 3 18.5z"/></svg>`,
};

/// The longest side a picture is sent at, and the most bytes. 1568 pixels is
/// the size past which Claude shrinks a picture itself -- sending more only
/// costs time on the wire -- and five megabytes is the most it takes at all.
const PICTURE_SIDE = 1568;
const PICTURE_BYTES = 3.5 * 1024 * 1024;

state.attached = { chat: [], cook: [] };
// The one small menu open over the box, as "<which>:<mode>" -- "attach:chat",
// "route:cook" -- or null. One at a time, and any click outside it shuts it.
state.menu = null;

/// Which box the menu and the tiles belong to: the view's.
function attachMode() {
  return state.view === 'cook' ? 'cook' : 'chat';
}

/// The tiles, in the order they were added. Nothing when there are none, so
/// an empty box is as small as it was before any of this.
function attachTiles(mode) {
  const items = state.attached[mode] || [];
  if (!items.length) return '';
  return `<div class="tiles">${items.map((item, index) => tileView(item, index, mode)).join('')}</div>`;
}

/// One tile. A picture is itself; a PDF is a page with the start of its text
/// on it; anything else is its name and what kind of thing it is.
function tileView(item, index, mode) {
  const shut = `<button type="button" class="tile-x" data-act="attach-remove" data-mode="${mode}"
      data-index="${index}" title="Remove" aria-label="Remove ${escape(item.name)}">${CLIP.shut}</button>`;
  const badge = `<span class="badge">${escape(item.label || 'FILE')}</span>`;
  const tip = [item.path, item.bytes ? bytes(item.bytes) : '', item.error || ''].filter(Boolean).join('\n');
  let inner;
  let kind = 'doc';
  if (item.loading) {
    inner = `<div class="tile-name">${escape(item.name)}</div><div class="tile-meta">reading...</div>`;
  } else if (item.kind === 'image' && item.thumb) {
    kind = 'picture';
    inner = `<img src="${item.thumb}" alt="${escape(item.name)}">`;
  } else if (item.kind === 'document' && item.label === 'PDF' && item.preview) {
    kind = 'page';
    inner = `<div class="page"><div class="page-text">${escape(item.preview)}</div></div>${badge}`;
  } else if (item.kind === 'folder') {
    inner = `<div class="tile-icon">${CLIP.box}</div><div class="tile-name">${escape(item.name)}</div>
      <div class="tile-meta">${count(item.files || 0, 'file')}</div>${badge}`;
  } else {
    inner = `<div class="tile-name">${escape(item.name)}</div>${
      item.error ? `<div class="tile-meta bad">${escape(item.error)}</div>` : ''}${badge}`;
  }
  return `<div class="tile ${kind}${item.error ? ' wrong' : ''}" title="${escape(tip)}">${inner}${shut}</div>`;
}

/// What a past prompt had attached, small, above what was typed with it.
function attachedChips(list) {
  if (!list || !list.length) return '';
  return `<div class="attached">${list.map((one) =>
    `<span class="attached-one" title="${escape(one.path || one.name)}"><span class="badge">${
      escape(one.label || 'FILE')}</span>${escape(one.name)}</span>`).join('')}</div>`;
}

/// The plus, and the menu it opens. Up from the box rather than down from
/// it, because the box is at the bottom of the window.
function attachButton(mode, disabled) {
  const open = state.menu === `attach:${mode}` && !disabled;
  const menu = open ? `<div class="popmenu" role="menu">
      <button type="button" role="menuitem" data-act="attach-files">${CLIP.file}
        <span>Add files or photos</span><kbd>Ctrl+U</kbd></button>
      <button type="button" role="menuitem" data-act="attach-folder">${CLIP.folder}
        <span>Add folder</span></button></div>` : '';
  return `<div class="menu-wrap">
      <button type="button" class="plus" data-act="attach-menu" aria-haspopup="menu"
              aria-expanded="${open}" title="Attach files, photos or a folder"
              aria-label="Attach files, photos or a folder" ${disabled ? 'disabled' : ''}>${CLIP.plus}</button>
      ${menu}</div>`;
}

/// Open or shut one of the box's menus: `which` is "attach", "route" or
/// "effort".
function toggleMenu(which) {
  const key = `${which}:${attachMode()}`;
  state.menu = state.menu === key ? null : key;
  render();
}
actions['attach-menu'] = () => toggleMenu('attach');
actions['attach-files']  = () => attachPick(false);
actions['attach-folder'] = () => attachPick(true);
actions['attach-remove'] = (button) => {
  const list = state.attached[button.dataset.mode] || [];
  list.splice(Number(button.dataset.index), 1);
  render();
};

/// Ask the platform's dialog for files, or a folder, and add what it gives.
async function attachPick(folder) {
  const mode = attachMode();
  state.menu = null;
  render();
  const box = document.getElementById('prompt');
  if (!box || box.disabled) return;
  const paths = await pickPaths({
    folder, multiple: !folder, start: state.attachedFrom || '',
    title: folder ? 'Attach a folder' : 'Attach files or photos' });
  if (paths.length) {
    state.attachedFrom = folder ? paths[0] : paths[0].replace(/[\\/][^\\/]*$/, '');
  }
  await attachPaths(mode, paths);
  const again = document.getElementById('prompt');
  if (again) again.focus();
}

/// Put `paths` in the box: a tile for each at once, filled in as the session
/// says what each one is.
async function attachPaths(mode, paths) {
  const list = state.attached[mode];
  const fresh = paths.filter((path) => path && !list.some((item) => item.path === path));
  if (!fresh.length) return;
  for (const path of fresh) {
    list.push(waitingTile(path, path.replace(/[\\/]+$/, '').split(/[\\/]/).pop() || path, false));
  }
  render();
  await inspectTiles(mode, fresh);
}

/// A tile for something not yet looked at: its name, and "reading...".
function waitingTile(path, name, folder) {
  const dot = name.lastIndexOf('.');
  return { path, name, kind: folder ? 'folder' : 'file', loading: true,
           label: folder ? 'FOLDER' : dot > 0 ? name.slice(dot + 1).toUpperCase() : 'FILE' };
}

/// Fill in the tiles for `paths` with what the session says each one is.
async function inspectTiles(mode, paths) {
  const list = state.attached[mode];
  await guard(async () => {
    const reply = await call('attach.inspect', { paths });
    for (const info of reply.items || []) {
      const at = list.findIndex((item) => item.path === info.path);
      if (at < 0) continue;   // taken out again while it was being looked at
      if (info.kind === 'missing') {
        list.splice(at, 1);
        state.error = `${info.name} is not there`;
        continue;
      }
      Object.assign(list[at], info, { loading: false });
      if (info.kind === 'image') loadPicture(list[at]);
    }
  });
}

/// A picture's thumbnail, and the version of it that is sent.
async function loadPicture(item) {
  try {
    const picture = await call('attach.image', { path: item.path });
    const image = await decodePicture(`data:${picture.mime};base64,${picture.data}`);
    item.thumb = redraw(image, 320, 'image/jpeg', 0.82).url;
    item.image = forSending(image, picture);
  } catch (error) {
    item.error = error.message || 'it could not be read as a picture';
  }
  render();
}

function decodePicture(source) {
  return new Promise((resolve, reject) => {
    const image = new Image();
    image.onload = () => resolve(image);
    image.onerror = () => reject(new Error('it could not be read as a picture'));
    image.src = source;
  });
}

/// `image` drawn again with its longest side at most `side`, as `mime`.
function redraw(image, side, mime, quality) {
  const scale = Math.min(1, side / Math.max(image.naturalWidth, image.naturalHeight));
  const canvas = document.createElement('canvas');
  canvas.width = Math.max(1, Math.round(image.naturalWidth * scale));
  canvas.height = Math.max(1, Math.round(image.naturalHeight * scale));
  const context = canvas.getContext('2d');
  if (mime === 'image/jpeg') {
    // A JPEG has no transparency, and what was transparent comes out black.
    context.fillStyle = '#ffffff';
    context.fillRect(0, 0, canvas.width, canvas.height);
  }
  context.drawImage(image, 0, 0, canvas.width, canvas.height);
  const url = canvas.toDataURL(mime, quality);
  return { url, data: url.slice(url.indexOf(',') + 1) };
}

/// The picture as it is sent: as it is when it is already small enough and
/// in a format every provider takes, and shrunk to a JPEG when it is not.
function forSending(image, picture) {
  const taken = ['image/png', 'image/jpeg', 'image/gif', 'image/webp'].includes(picture.mime);
  const side = Math.max(image.naturalWidth, image.naturalHeight);
  if (taken && side <= PICTURE_SIDE && picture.data.length * 0.75 <= PICTURE_BYTES) {
    return { mime: picture.mime, data: picture.data };
  }
  // A screenshot is sharper as a PNG; a photograph is far smaller as a JPEG.
  // Which one this is shows in which comes out smaller.
  let best = redraw(image, PICTURE_SIDE, 'image/jpeg', 0.86);
  let mime = 'image/jpeg';
  if (picture.mime === 'image/png') {
    const png = redraw(image, PICTURE_SIDE, 'image/png');
    if (png.data.length < best.data.length * 1.5 && png.data.length * 0.75 <= PICTURE_BYTES) {
      best = png;
      mime = 'image/png';
    }
  }
  return { mime, data: best.data };
}

/// What is attached in `mode`, in the shape submit and cook.start take.
/// Null while anything is still being read, which the send waits on.
function attachmentsFor(mode) {
  const list = state.attached[mode] || [];
  if (list.some((item) => item.loading || (item.kind === 'image' && !item.image && !item.error))) {
    return null;
  }
  return list.map((item) => (item.image ? { path: item.path, image: item.image } : { path: item.path }));
}

/// A menu closes on a click anywhere else, and on Escape.
document.addEventListener('click', (event) => {
  if (state.menu && !(event.target.closest && event.target.closest('.menu-wrap'))) {
    state.menu = null;
    render();
  }
}, true);
document.addEventListener('keydown', (event) => {
  if (event.key === 'Escape' && state.menu) {
    state.menu = null;
    render();
  }
});

// --- dropped on the window -----------------------------------------------------------
//
// Anything dragged over the window is offered the box: the window goes soft
// behind an outline in the flame, and what is let go of is attached as if it
// had been chosen with the plus. On Chat or Cook it goes in that box; from
// any other view, in Chat's, which the drop then opens.
//
// A page is handed a dropped file's contents and never its path -- and on
// WebKitGTK not even the contents. Where the window can say the paths
// (Linux, see src/gui/drops.hpp), it does, and they are attached as they
// are. Where it cannot -- WebView2 and WKWebView -- the page sends what it
// was handed, a piece at a time, and Crucible keeps a copy in its own
// folder, which is what is attached. A folder is walked here then, skipping
// what a folder attached through the dialog skips, and up to a limit: a
// dropped folder is read into memory to be copied, and a drop is not an
// import.

const DROP_SKIP = new Set(['node_modules', 'build', 'dist', 'target', 'out', '__pycache__', 'venv',
  'vendor', 'bin', 'obj', 'Pods', 'DerivedData', 'coverage']);
const DROP_PICTURES = /\.(png|jpe?g|gif|webp|bmp)$/i;
const DROP_MAX_FILES = 2000;
const DROP_MAX_BYTES = 256 * 1024 * 1024;   // a whole drop
const DROP_FILE_MAX = 64 * 1024 * 1024;     // one file, the most that is ever read of one
const DROP_INNER_MAX = 16 * 1024 * 1024;    // one file inside a folder
const DROP_PIECE = 3 * 1024 * 1024;         // sent at a time

CLIP.drop = `<svg viewBox="0 0 24 24" width="46" height="46" aria-hidden="true" fill="none" stroke="currentColor"
  stroke-width="1.4" stroke-linecap="round" stroke-linejoin="round"><path d="M12 3v12M7 10l5 5 5-5"/>
  <path d="M4 15v3.5A1.5 1.5 0 0 0 5.5 20h13a1.5 1.5 0 0 0 1.5-1.5V15"/></svg>`;

/// Which box a drop would go in, or why it cannot go in one: the same
/// reasons the box itself is shut for.
function dropTarget() {
  const s = state.snapshot || {};
  if (state.modal) return { why: 'Close the dialog first' };
  const cook = s.cook && s.cook.running ? s.cook : null;
  if (cook) {
    return { why: cook.state === 'asking' ? 'Answer the cook\'s question first'
                                          : 'A cook is running -- it has the experts' };
  }
  return { mode: state.view === 'cook' ? 'cook' : 'chat' };
}

/// The middle of the overlay: what a drop will do, or why it will not.
function dropView(target) {
  return `<div class="drop-frame"></div><div class="drop-words">${CLIP.drop}<div>${
    target.mode ? 'Drop files or folders here' : escape(target.why)}</div>${
    target.mode && target.mode !== state.view ? '<div class="drop-why">They go in the box on Chat</div>'
                                              : ''}</div>`;
}

/// Whether a drag may be carrying files, rather than text -- which a text box
/// takes as it always has. WebView2 and WKWebView say "Files"; WebKitGTK says
/// only that it is a list of URIs, which a link dragged from a browser is too.
/// Which it is cannot be read until the drop, and a link let go of here then
/// attaches nothing -- which beats the window going off to load it.
function carriesFiles(event) {
  const types = event.dataTransfer ? Array.from(event.dataTransfer.types || []) : [];
  return types.includes('Files') || types.includes('text/uri-list');
}

let dropHiding = 0;
function dropOver(event) {
  if (!carriesFiles(event)) return;
  // Taken, always: a file let go of on a page the page did not take is
  // opened in its place, and the window would be showing a PDF instead.
  event.preventDefault();
  clearTimeout(dropHiding);
  const target = dropTarget();
  event.dataTransfer.dropEffect = target.mode ? 'copy' : 'none';
  const overlay = document.getElementById('drop');
  const said = target.why || target.mode;
  if (overlay && (!document.body.classList.contains('dropping') || overlay.dataset.said !== said)) {
    overlay.innerHTML = dropView(target);
    overlay.dataset.said = said;
    overlay.classList.toggle('shut', !target.mode);
    document.body.classList.add('dropping');
  }
}

function dropOff() {
  clearTimeout(dropHiding);
  document.body.classList.remove('dropping');
}

window.addEventListener('dragenter', dropOver, true);
window.addEventListener('dragover', dropOver, true);
// Leaving one element is entering the next, and the enter cancels this. Only
// leaving the window is followed by nothing.
window.addEventListener('dragleave', () => {
  clearTimeout(dropHiding);
  dropHiding = setTimeout(dropOff, 80);
}, true);
window.addEventListener('dragend', dropOff, true);
window.addEventListener('drop', (event) => {
  if (!carriesFiles(event)) return;
  event.preventDefault();
  dropOff();
  const target = dropTarget();
  if (!target.mode) return;
  // What the page is handed can be read only while the event lasts, so it
  // is all taken now, before anything is waited for.
  const transfer = event.dataTransfer;
  const entries = Array.from(transfer.items || [])
    .map((item) => (item.kind === 'file' && item.webkitGetAsEntry ? item.webkitGetAsEntry() : null))
    .filter(Boolean);
  const tops = entries.length ? entries : Array.from(transfer.files || []).map((file) => ({
    isFile: true, isDirectory: false, name: file.name, file: (ok) => ok(file) }));
  if (state.view !== target.mode) enter(target.mode);
  takeDrop(target.mode, tops);
}, true);

// --- the paths, where the window says them ---------------------------------------------

let heardPaths = null;      // paths the window sent before the page's drop arrived
let awaitingPaths = null;   // the page's drop, waiting for the window's paths
let tookPathsAt = 0;

/// Called by the window with the paths of what was just dropped.
window.crucibleDropped = (paths) => {
  if (awaitingPaths) {
    const done = awaitingPaths;
    awaitingPaths = null;
    done(paths);
    return;
  }
  heardPaths = { paths, at: Date.now() };
  // Before the page's own drop, or with none: give that a moment to arrive,
  // then take the paths without it.
  setTimeout(() => {
    if (!heardPaths || heardPaths.paths !== paths) return;
    heardPaths = null;
    const target = dropTarget();
    if (!target.mode) return;
    tookPathsAt = Date.now();
    if (state.view !== target.mode) enter(target.mode);
    attachPaths(target.mode, paths);
  }, 600);
};

function windowPaths() {
  if (heardPaths && Date.now() - heardPaths.at < 2000) {
    const { paths } = heardPaths;
    heardPaths = null;
    return Promise.resolve(paths);
  }
  return new Promise((resolve) => {
    awaitingPaths = resolve;
    setTimeout(() => {
      if (awaitingPaths === resolve) {
        awaitingPaths = null;
        resolve(null);
      }
    }, 1500);
  });
}

async function takeDrop(mode, tops) {
  if (Date.now() - tookPathsAt < 3000) return;   // the window's paths got here first
  const paths = window.crucibleNativeDrops ? await windowPaths() : null;
  if (paths && paths.length) {
    tookPathsAt = Date.now();
    return attachPaths(mode, paths);
  }
  if (tops.length) return keepDropped(mode, tops);
}

// --- copies, where it does not ----------------------------------------------------------

/// Send what was dropped to be kept, and attach the copies.
async function keepDropped(mode, tops) {
  const batch = Date.now().toString(36) + Math.random().toString(36).slice(2, 8);
  const list = state.attached[mode];
  const tiles = tops.map((top) => {
    const tile = waitingTile(`dropping:${batch}/${top.name}`, top.name, top.isDirectory);
    list.push(tile);
    return tile;
  });
  render();
  const budget = { files: 0, bytes: 0, left: [] };
  const kept = [];
  let failed = '';
  for (let i = 0; i < tops.length; i += 1) {
    const tile = tiles[i];
    let path = null;
    try {
      path = await keepEntry(batch, tops[i], tops[i].name, budget, true);
    } catch (error) {
      failed = `${tops[i].name}: ${error.message || 'it could not be read'}`;
    }
    const at = list.indexOf(tile);
    if (at < 0) continue;   // taken out again while it was being copied
    if (!path) {
      list.splice(at, 1);
      continue;
    }
    tile.path = path;
    kept.push(path);
  }
  if (kept.length) await inspectTiles(mode, kept);
  // Said after the tiles are filled in, which clears what was said before.
  if (budget.left.length) {
    failed = `Too large to drop: ${budget.left.slice(0, 3).join(', ')}${
      budget.left.length > 3 ? ` and ${budget.left.length - 3} more` : ''}. Use the plus.`;
  }
  if (failed) state.error = failed;
  render();
}

/// Keep one dropped thing, a folder's contents and all. Returns where it was
/// kept, or null when it was left out.
async function keepEntry(batch, entry, relative, budget, top) {
  if (entry.isDirectory) {
    const made = await call('attach.store', { batch, path: relative, folder: true, data: '' });
    const children = (await childrenOf(entry)).sort((a, b) => (a.name < b.name ? -1 : 1));
    for (const child of children) {
      if (child.name.startsWith('.') || DROP_SKIP.has(child.name)) continue;
      // Pictures in a folder are not read, so they are not copied either.
      if (child.isFile && DROP_PICTURES.test(child.name)) continue;
      if (budget.files >= DROP_MAX_FILES) break;
      await keepEntry(batch, child, `${relative}/${child.name}`, budget, false);
    }
    return made.path;
  }
  const file = await new Promise((resolve, reject) => entry.file(resolve, reject));
  if (file.size > (top ? DROP_FILE_MAX : DROP_INNER_MAX) || budget.bytes + file.size > DROP_MAX_BYTES) {
    budget.left.push(relative);
    return null;
  }
  budget.files += 1;
  budget.bytes += file.size;
  let path = null;
  for (let at = 0; at === 0 || at < file.size; at += DROP_PIECE) {
    const bytes = new Uint8Array(await file.slice(at, at + DROP_PIECE).arrayBuffer());
    const reply = await call('attach.store', { batch, path: relative, data: base64Of(bytes), append: at > 0 });
    path = reply.path;
  }
  return path;
}

/// Everything in a dropped folder. readEntries hands them over a batch at a
/// time and says it is finished with an empty one.
function childrenOf(entry) {
  return new Promise((resolve, reject) => {
    const reader = entry.createReader();
    const all = [];
    const next = () => reader.readEntries((some) => {
      if (!some.length) return resolve(all);
      all.push(...some);
      return next();
    }, reject);
    next();
  });
}

function base64Of(bytes) {
  let text = '';
  for (let at = 0; at < bytes.length; at += 0x8000) {
    text += String.fromCharCode.apply(null, bytes.subarray(at, at + 0x8000));
  }
  return btoa(text);
}
