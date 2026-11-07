/* DevilzDad's ESM Shop + Explorer - OSF UI view.
 *
 * The native plugin (DevilzDadShopOSF.dll) owns the catalog, the classifiers,
 * the basket and the delivery. This page asks for a page of rows and draws it;
 * it never decides what is buyable, so it cannot drift from the overlay build.
 *
 * Authoritative code: the catalog, basket and delivery live in
 * SF-Shop/SF Shop Explorer/src/OsfFrontend.cpp (the endpoints) and
 * ShopExplorer.cpp (ShopBrowser, which both front ends call). Favourites are
 * stored by ShopBrowser::ToggleFavourite in SFShopExplorerFavourites.txt, shared
 * with the overlay build. This page only holds a copy for display.
 */
'use strict';

const MOD = 'devilzdad.osfshop';
const VIEW_ID = MOD + '/shop';
// Answers arrive as a typed push carrying { id, payload }; the reply half of
// the request pair is kept as a second chance (see settleEnvelope).
const REPLY_TYPE = MOD + '.reply';
const EP = {
  catalog: MOD + '.catalog',
  rows: MOD + '.rows',
  basket: MOD + '.basket',
  accept: MOD + '.accept',
  detail: MOD + '.detail',
  configure: MOD + '.configure',
  refresh: MOD + '.refresh',
  preview: MOD + '.preview',
  audio: MOD + '.audio',
  themes: MOD + '.themes',
  themepick: MOD + '.themepick',
  customtheme: MOD + '.customtheme',
  backdrop: MOD + '.backdrop',
  favourite: MOD + '.favourite',
};

// One path for the header star and every row star, so a starred row and the
// column heading cannot drift apart.
const STAR_PATH = '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M12 3.6l2.6 5.6 6.1.7-4.5 4.2 1.2 6-5.4-3.1-5.4 3.1 1.2-6L3.3 9.9l6.1-.7z"/></svg>';
const PAGE = 100;
const MAX_LINES = 25;

// Bump this whenever shop.css, app.js or the shipped backdrop art changes. The
// view URL itself never changes - OSF serves every legacy view from the same
// virtual host - so without it WebView2 is free to answer with the page it
// cached before the fix, which is exactly how the old 135%-and-blur CSS kept
// coming back after it had been replaced on disk.
const ASSET_VERSION = '20261031b';

// Row type labels the engine sends, mapped to the chip classes in shop.css.
const TYPE_CLASS = {
  Weapons: 'WEAP', Armor: 'ARMO', Ammo: 'AMMO', Aid: 'ALCH',
  Books: 'BOOK', Keys: 'KEYM', Misc: 'MISC', Ingredients: 'INGR',
};

// Column headings, in the order index.html lays them out. The numbers are the
// protocol: the plugin sorts on these, so the two lists must stay in step.
const SORT_LABELS = ['Name', 'Shelf', 'Type', 'Value', 'Weight', 'FormID'];

const state = {
  plugins: [],
  categories: [],
  filter: { type: 8, sub: 0, plugin: -1, hideTest: true, hideHair: true, hideCrew: true, hideTurrets: true, hideBug: true, hideDuplicates: true },
  search: '',
  // Header clicked: -1 is catalogue order, which is what the overlay shows
  // until a column is picked. Ascending matches the overlay's first click.
  sort: { column: -1, ascending: true },
  // Set once the first catalog reply has been read, so a later re-sync (focus
  // or visibility) cannot undo a header click with the stored value.
  sortRestored: false,
  page: 0,
  total: 0,
  rows: [],
  selected: null,
  detail: null,
  basket: [],
  credits: 0,
  cost: 0,
  free: true,
  version: '',
  // Starred record ids, and whether the item pane is showing only those. The
  // list is the overlay's: both front ends write the same file.
  favourites: [],
  favouritesOnly: false,
  busy: false,
  booted: false,
  retries: 0,
};

const el = {
  search: document.getElementById('search'),
  searchBtn: document.getElementById('searchBtn'),
  credits: document.getElementById('credits'),
  refresh: document.getElementById('refresh'),
  categories: document.getElementById('categories'),
  subs: document.getElementById('subs'),
  pluginList: document.getElementById('pluginList'),
  pluginCount: document.getElementById('pluginCount'),
  rows: document.getElementById('rows'),
  tblhdr: document.querySelector('.tblhdr'),
  pager: document.getElementById('pager'),
  detail: document.getElementById('detail'),
  basketItems: document.getElementById('basketItems'),
  basketCount: document.getElementById('basketCount'),
  totalCost: document.getElementById('totalCost'),
  free: document.getElementById('free'),
  acceptBtn: document.getElementById('acceptBtn'),
  clearBtn: document.getElementById('clearBtn'),
  status: document.getElementById('status'),
  footnote: document.getElementById('footnote'),
  audioBtn: document.getElementById('audioBtn'),
  themeBtn: document.getElementById('themeBtn'),
  hideBtn: document.getElementById('hideBtn'),
  favouritesOnly: document.getElementById('favouritesOnly'),
  favCount: document.getElementById('favCount'),
};

const popups = {
  audio: document.getElementById('audioPop'),
  theme: document.getElementById('themePop'),
  custom: document.getElementById('customPop'),
  hide: document.getElementById('hidePop'),
};

const preview = {
  yaw: 0,
  pitch: 0,
  zoom: 1,
  busy: false,
  queued: false,
  token: 0,
  timer: 0,
  drag: null,
  moves: 0,
  asked: '',
};

const pv = {
  box: document.getElementById('preview'),
  img: document.getElementById('previewImg'),
  empty: document.getElementById('previewEmpty'),
  hint: document.getElementById('previewHint'),
};

const audio = {
  musicVolume: 45,
  sfxVolume: 55,
  musicMuted: false,
  sfxMuted: false,
  track: '',
  tracks: [],
  // True while the track names a soundtrack folder; the step buttons need it.
  playlist: false,
};

const themes = { rows: [], active: 0 };

// The custom theme's draft, filled from the plugin and written back on Save.
const custom = { name: 'Custom', suffix: '', colours: {} };

const HIDES = {
  hideTest: document.getElementById('hideTest'),
  hideBug: document.getElementById('hideBug'),
  hideHair: document.getElementById('hideHair'),
  hideCrew: document.getElementById('hideCrew'),
  hideTurrets: document.getElementById('hideTurrets'),
  // Not a class of record like the others: the game really does define some
  // items twice (a master's item beside a mod's clone of it), so this only
  // collapses them to one row. The row keeps the count and the item pane lists
  // the records it stands for.
  hideDuplicates: document.getElementById('hideDuplicates'),
};

/* ---------------------------------------------------------------- bridge -- */

function bridge() {
  return window.osfui || null;
}

function bridgeReady() {
  const g = bridge();
  return !!(g && typeof g.postMessage === 'function');
}

/* The wire, not the helper.
 *
 * OSF UI ships two `shared/osfui.js` kits: the 2.0 one (OSF/UI/views) and a 1.6
 * one adapted to the v2 transport (OSFUI/views). They disagree about what a
 * `request()` promise resolves with, and - as this build proved - the reply a
 * legacy-root view gets back does not always match the helper that was loaded,
 * so every request sat unresolved until its 10 s timeout while the plugin had
 * already answered. Requests are therefore posted and settled here, in the
 * envelope the runtime documents ({kind, name, id, payload} in,
 * {kind, id, payload} out, plus the 1.6 {type, requestId} spelling), and the
 * helper only keeps what it is good at.
 */
const inflight = new Map();
let requestSeq = 0;

// Settles one of our requests from a decoded message. Returns true when the
// message belonged to us. Both transports land here: the runtime's
// { kind:'reply' | 'error', id, payload } and the plugin's own
// { id, payload } push, because the reply half does not reach this view on
// OSF UI 2.0 (the plugin logged its answer while every page request timed
// out). Whichever arrives first wins; the other finds the id already gone.
function settleEnvelope(message) {
  if (!message || typeof message !== 'object') return false;
  const id = message.id || message.requestId || '';
  if (!id || !inflight.has(id)) return false;
  const call = inflight.get(id);
  inflight.delete(id);
  if (call.timer) clearTimeout(call.timer);
  let payload = Object.prototype.hasOwnProperty.call(message, 'payload') ? message.payload : message;
  if (typeof payload === 'string') {
    try { payload = JSON.parse(payload); } catch (err) { /* keep the text */ }
  }
  const info = (payload && typeof payload === 'object') ? payload : {};
  // Only the transport rejects a request. A domain refusal - "basket is full",
  // "theme is locked" - resolves with ok:false, because those answers carry
  // state the page has to keep, and every caller already prints reply.error.
  const failed = message.kind === 'error' || message.type === 'ui.error' || message.ok === false ||
    (info.ok === false && !!info.code);
  if (failed) {
    const err = new Error(info.message || info.error || info.code || message.message || 'request failed');
    err.code = info.code || '';
    call.reject(err);
  } else {
    call.resolve(payload);
  }
  return true;
}

function postRaw(envelope) {
  const g = bridge();
  if (!g || typeof g.postMessage !== 'function') return false;
  try {
    g.postMessage(JSON.stringify(envelope));
    return true;
  } catch (err) {
    return false;
  }
}

// Settles one of our requests from a raw runtime message. Returns true when the
// message belonged to us.
function settleIncoming(raw) {
  let message = raw;
  if (typeof raw === 'string') {
    try { message = JSON.parse(raw); } catch (err) { return false; }
  }
  return settleEnvelope(message);
}

// The plugin pushes every answer to this view as `devilzdad.osfshop.reply`,
// which the loaded helper delivers to osfui.on subscribers. That path is
// measured working on OSF UI 2.0 (it is how the shipped views receive state),
// so it is the one the page relies on.
function hookReplies() {
  const g = bridge();
  if (!g || typeof g.on !== 'function') return;
  g.on(REPLY_TYPE, (payload) => {
    try { settleEnvelope(payload); } catch (err) { /* never break delivery */ }
  });
}

// Called once the runtime exists: our settler runs first, then whatever the
// loaded helper does with the same message.
function hookMessages() {
  const g = bridge();
  if (!g || g.__shopHooked) return;
  const previous = g.onMessage;
  g.onMessage = function (json) {
    try { settleIncoming(json); } catch (err) { /* never break delivery */ }
    if (typeof previous === 'function') return previous.call(this, json);
    return undefined;
  };
  g.__shopHooked = true;
}

function call(name, payload, timeoutMs) {
  const g = bridge();
  if (!g || typeof g.postMessage !== 'function') {
    return Promise.reject(new Error('OSF UI bridge is not available'));
  }
  const id = 'shop' + (++requestSeq);
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => {
      inflight.delete(id);
      reject(new Error(name + ' got no reply within ' + (timeoutMs || 10000) + 'ms'));
    }, timeoutMs || 10000);
    inflight.set(id, { resolve, reject, timer });
    // The id also rides inside the payload: the plugin's push reply has no
    // other way to name the request it answers.
    const body = Object.assign({}, payload || {}, { requestId: id });
    if (!postRaw({ kind: 'request', name: name, id: id, payload: body })) {
      inflight.delete(id);
      clearTimeout(timer);
      reject(new Error('OSF UI bridge refused the request'));
    }
  });
}

function send(name, payload) {
  postRaw({ kind: 'send', name: name, payload: payload || {} });
}

// UI sounds live in the plugin: it owns the decoded bank and the same volume /
// mute settings the Audio panel edits, so the page only names the sound. This
// was missing entirely - the OSF page was silent while the overlay clicked -
// and music worked, which read as "sound is broken" rather than "not wired".
let sfxHoverAt = 0;
let sfxWarned = false;
function sfx(name) {
  if (name === 'hover') {
    // A mouse sweep across twenty rows is not twenty voices, and PlaySfx
    // steals voices: throttle so a sweep is a flicker of clicks, not a buzz.
    const now = Date.now();
    if (now - sfxHoverAt < 90) return;
    sfxHoverAt = now;
  }
  // A request, not a send: `send` posts kind:'send', which the host dispatches
  // to RegisterCommand handlers, and the audio endpoint is a RegisterRequest
  // one. Sending it that way looked wired and was silently dropped, which is
  // why only the plugin's own open/close stings were audible. A sound never
  // needs its reply, so the catch is the whole error policy.
  // One warning per session, not per click: if the route ever breaks again,
  // the plugin log says so once instead of staying silent until someone
  // notices the shop has no sound.
  call(EP.audio, { op: 'sfx', name: name }, 2000).catch((err) => {
    if (sfxWarned) return;
    sfxWarned = true;
    reportToNative('sfx \'' + name + '\' got no answer: ' + (err && err.message ? err.message : err));
  });
}

// A WebView cannot write a log file, so anything the page fails at is invisible
// from outside the game. This mirrors it into the plugin's own log (one line per
// message) where a report can be read. Guarded against recursion and against a
// bridge that refuses the endpoint.
let reporting = false;

function reportToNative(text) {
  if (reporting) return;
  reporting = true;
  try {
    send(MOD + '.note', { text: String(text == null ? '' : text).slice(0, 400) });
  } catch (err) {
    /* diagnostics must never break the page */
  } finally {
    reporting = false;
  }
}

/* ------------------------------------------------------------- lifecycle -- */

// OSF opens this view (its interface list or the key bound in OSF Settings)
// and closes it (Escape/Back, the close button, pad B). The plugin does not
// poll a key of its own, so the engine session - music, the muted game
// session, the `open` flag - follows what OSF actually did to the view. OSF
// publishes that in its own `views` state; a hidden document is the same fact
// from the WebView side, which also covers a destroyed page.
let viewVisible = false;

function reportViewVisible(visible) {
  if (visible === viewVisible) return;
  viewVisible = visible;
  // One line per real transition, so the plugin log shows whether OSF's state
  // reached this page at all - the thing that was invisible while the key was
  // wrong and the music kept playing over a closed shop.
  reportToNative('view ' + (visible ? 'shown' : 'hidden') + '; reporting to the plugin');
  send(MOD + '.view', { open: visible });
}

function watchViewState() {
  const g = bridge();
  // 2.0 names this channel `state`, the 1.6 kit names it `data`; both replay the
  // current value on subscribe, which is the whole point of using it.
  const channel = g && g.state && typeof g.state.on === 'function' ? g.state
    : g && g.data && typeof g.data.on === 'function' ? g.data : null;
  if (channel) {
    // The native side publishes this view's state with mod `osfui` and key
    // `views` as separate fields (MessageBridge::EncodeState), and the 1.6 kit
    // matches `payload.key` literally - so the bare key is the one that fires.
    // A 2.0 kit qualifies keys with the owning mod, hence both keys: whichever
    // kit is served, one of them matches, and a repeated report is idempotent
    // (reportViewVisible only sends on a real transition).
    const onViews = (value) => {
      const views = Array.isArray(value) ? value : (value && Array.isArray(value.views) ? value.views : []);
      const self = views.find((entry) => entry && entry.id === VIEW_ID);
      const open = !!(self && self.open);
      if (!self) { reportViewVisible(false); return; }
      reportViewVisible(open);
    };
    channel.on('views', onViews);
    channel.on('osfui/views', onViews);
  }
  document.addEventListener('visibilitychange', () => {
    if (document.hidden) reportViewVisible(false);
  });
  window.addEventListener('pagehide', () => reportViewVisible(false));
}

function say(text, kind) {
  el.status.textContent = text || '';
  el.status.className = kind || '';
  // One place for a refusal: every rejected action already routes through
  // here with 'err', so the denied sting cannot drift from the message.
  if (kind === 'err' && text) {
    sfx('denied');
    reportToNative(text);
  }
}

function esc(value) {
  return String(value == null ? '' : value)
    .replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;').replace(/'/g, '&#39;');
}

function nameOf(item) {
  return item.name && item.name.length ? item.name : '[' + item.formId + ']';
}

/* -------------------------------------------------------------- catalog -- */

function catalogFailed(message) {
  // The plugin panel is the one place that is empty when this happens, so say
  // why there rather than only in a status line at the bottom of the screen.
  say(message, 'err');
  el.pluginList.innerHTML = '<div class="prow">' + esc(message) + '</div>';
}

function applyCatalog(reply) {
  if (!reply || typeof reply !== 'object' || reply.ok === false || !Array.isArray(reply.plugins)) {
    const shape = reply && typeof reply === 'object' ? Object.keys(reply).join(',') : typeof reply;
    catalogFailed('Catalog unavailable: ' + ((reply && reply.error) || ('unexpected reply (' + shape + ')')));
    return;
  }
  // The plugin keeps this page's sort in its INI (`[shop] osfSortColumn` /
  // `osfSortAscending`) and sends it with the catalog. Adopt it on the first
  // reply of a session only: every other reply is a re-sync, and those must not
  // overwrite the column the player just clicked.
  if (!state.sortRestored) {
    state.sortRestored = true;
    if (typeof reply.sortColumn === 'number') {
      const column = Number(reply.sortColumn);
      state.sort.column = column >= 0 && column <= 5 ? column : -1;
      state.sort.ascending = reply.sortAscending !== false;
      renderSort();
    }
  }
  state.plugins = reply.plugins || [];
  state.categories = reply.categories || [];
  state.credits = reply.credits || 0;
  state.basket = reply.basket || [];
  state.cost = reply.cost || 0;
  state.version = reply.version || '';
  // The open key OSF bound for this view; the page closes the shop on it too.
  state.openKey = typeof reply.openKey === 'string' ? reply.openKey : '';
  if (reply.filter) state.filter = Object.assign({}, state.filter, reply.filter);
  if (Array.isArray(reply.favourites)) state.favourites = reply.favourites;
  renderFavourites();
  // The plugin reports its own build label (for this mod, 0.8.350-OSF), so the
  // footnote is just that string: no second "OSF build" tag to drift from it.
  el.footnote.textContent = state.version ? 'v' + state.version : 'OSF build';
  renderPlugins();
  renderCategories();
  renderSubs();
  renderWallet();
  renderBasket();
}

async function loadCatalog() {
  if (state.busy) return;
  state.busy = true;
  try {
    applyCatalog(await call(EP.catalog, { free: state.free }));
    if (!state.plugins.length && state.retries < 40) {
      state.retries++;
      say('Waiting for the game to finish loading plugins... (' + state.retries + ')', '');
      setTimeout(() => { state.busy = false; loadCatalog(); }, 3000);
      return;
    }
    state.retries = 0;
    say('Loaded ' + state.plugins.length + ' plugins.');
    await loadRows(true);
  } catch (err) {
    catalogFailed('Failed to reach the shop: ' + errText(err));
  } finally {
    state.busy = false;
  }
}

async function loadRows(reset) {
  if (reset) state.page = 0;
  // favouritesOnly goes on last: state.filter is the plugin's echo of the
  // last filter, and merging it after the explicit keys would overwrite the
  // toggle with the stale value the catalog reply carried.
  const payload = Object.assign({
    offset: state.page * PAGE,
    limit: PAGE,
    search: state.search,
    free: state.free,
    sort: state.sort.column,
    asc: state.sort.ascending,
  }, state.filter, { favouritesOnly: state.favouritesOnly });
  try {
    const reply = await call(EP.rows, payload);
    if (!reply || reply.ok === false) {
      say('Catalog query failed: ' + ((reply && reply.error) || 'unknown error'), 'err');
      return;
    }
    state.total = reply.total || 0;
    state.rows = reply.rows || [];
    if (typeof reply.credits === 'number') state.credits = reply.credits;
    if (Array.isArray(reply.basket)) state.basket = reply.basket;
    if (typeof reply.cost === 'number') state.cost = reply.cost;
    if (Array.isArray(reply.favourites)) state.favourites = reply.favourites;
    renderFavourites();
    // The native side clamps the offset; trust its answer over the local page.
    if (state.total > 0 && state.page * PAGE >= state.total) {
      state.page = Math.max(0, Math.floor((state.total - 1) / PAGE));
      return loadRows(false);
    }
    renderRows();
    renderPager();
    renderWallet();
    renderBasket();
  } catch (err) {
    say('Catalog query failed: ' + errText(err), 'err');
  }
}

function errText(err) {
  if (!err) return 'unknown error';
  if (err.payload && err.payload.message) return err.payload.message;
  return err.message || String(err);
}

/* --------------------------------------------------------------- render -- */

function renderPlugins() {
  el.pluginCount.textContent = state.plugins.length ? '(' + state.plugins.length + ')' : '';
  const all = state.filter.plugin < 0;
  let html = '<div class="prow' + (all ? ' on' : '') + '" data-plugin="-1">' +
    '<span class="badge">ALL</span><span class="pname">All plugins</span></div>';
  state.plugins.forEach((plugin, index) => {
    html += '<div class="prow' + (state.filter.plugin === index ? ' on' : '') + '" data-plugin="' + index + '" title="' + esc(plugin.name) + '">' +
      '<span class="badge ' + esc(plugin.kind) + '">' + esc(plugin.kind) + '</span>' +
      '<span class="pname">' + esc(plugin.name) + '</span></div>';
  });
  el.pluginList.innerHTML = html;
}

function renderCategories() {
  let html = '';
  state.categories.forEach((category) => {
    html += '<button data-type="' + category.index + '"' + (state.filter.type === category.index ? ' class="on"' : '') + '>' +
      esc(category.name) + '</button>';
  });
  el.categories.innerHTML = html;
}

function currentCategory() {
  return state.categories.find((category) => category.index === state.filter.type) || null;
}

function renderSubs() {
  const category = currentCategory();
  if (!category) { el.subs.innerHTML = ''; return; }
  let html = '';
  category.subs.forEach((name, index) => {
    html += '<button data-sub="' + index + '"' + (state.filter.sub === index ? ' class="on"' : '') + '>' + esc(name) + '</button>';
  });
  el.subs.innerHTML = html;
}



function renderSort() {
  if (!el.tblhdr) return;
  SORT_LABELS.forEach((label, column) => {
    const cell = el.tblhdr.querySelector('[data-sort="' + column + '"]');
    if (!cell) return;
    const active = state.sort.column === column;
    cell.textContent = label + (active ? (state.sort.ascending ? ' \u25b2' : ' \u25bc') : '');
    cell.classList.toggle('sorted', active);
  });
}

function renderRows() {
  if (!state.rows.length) {
    const empty = state.favouritesOnly
      ? 'No starred records yet. Click the star at the left of any row to add one.'
      : 'No items match this view.';
    el.rows.innerHTML = '<p class="empty" style="color:#c79aa1;padding:8px">' + esc(empty) + '</p>';
    return;
  }
  let html = '';
  state.rows.forEach((row) => {
    const starred = isFavourite(row);
    html += '<div class="trow' + (state.selected === row.formId ? ' on' : '') + '" data-formid="' + esc(row.formId) + '">' +
      '<button class="fav' + (starred ? ' on' : '') + '" data-fav="' + esc(row.formId) + '" title="' +
      (starred ? 'Remove from favourites' : 'Add to favourites') + '">' + STAR_PATH + '</button>' +
      '<span class="name" title="' + esc(nameOf(row)) + '">' + esc(nameOf(row)) +
      (row.copies > 1 ? ' <span class="copies" title="' + row.copies +
        ' separate records share this name and these stats">&times;' + row.copies + '</span>' : '') + '</span>' +
      '<span class="bucket" title="' + esc(row.sub) + '">' + esc(row.sub) + '</span>' +
      '<span class="type"><span class="tchip ' + (TYPE_CLASS[row.type] || '') + '">' + esc(row.type) + '</span></span>' +
      '<span class="val">' + (row.value || 0) + '</span>' +
      '<span class="wt">' + (row.weight || 0) + '</span>' +
      '<span class="fid">' + esc(row.formId) + '</span></div>';
  });
  el.rows.innerHTML = html;
}

function paintSelection() {
  el.rows.querySelectorAll('.trow').forEach((node) => {
    node.classList.toggle('on', node.dataset.formid === state.selected);
  });
}

function renderPager() {
  const pages = Math.max(1, Math.ceil(state.total / PAGE));
  if (state.total <= 0) { el.pager.innerHTML = ''; return; }
  const from = state.page * PAGE + 1;
  const to = Math.min(state.total, (state.page + 1) * PAGE);
  el.pager.innerHTML =
    '<button data-page="0" title="First page">&#171;</button>' +
    '<button data-page="' + (state.page - 1) + '" title="Previous page">&#8249;</button>' +
    '<span>Page <b>' + (state.page + 1) + '</b> / ' + pages + ' &bull; ' + from + '&#8211;' + to + ' of ' + state.total + '</span>' +
    '<button data-page="' + (state.page + 1) + '" title="Next page">&#8250;</button>' +
    '<button data-page="' + (pages - 1) + '" title="Last page">&#187;</button>';
}

function renderWallet() {
  el.credits.textContent = state.credits;
  // Credits alone made a load-out that would not fit in cargo look free, so the
  // total carries the basket's mass beside the money.
  let mass = 0;
  state.basket.forEach((line) => { mass += (line.weight || 0) * line.count; });
  const money = state.free ? 'Free' : state.cost + ' credits';
  el.totalCost.textContent = mass > 0 ? money + '  \u00b7  ' + mass.toFixed(2) + ' wt' : money;
}


function renderDetail() {
  const detail = state.detail;
  if (!detail || !detail.ok) {
    el.detail.innerHTML = '<p class="empty">Select an item.</p>';
    pvFloatName.textContent = '';
    return;
  }
  const item = detail.item;
  // The floating window has no item table above it, so it carries the name.
  pvFloatName.textContent = nameOf(item) || '';
  let html = '<div class="dhead"><div class="glyph tchip ' + esc(detail.formType || '') + '" style="display:flex;align-items:center;justify-content:center;border-radius:8px">' +
    esc(detail.formType || '?') + '</div><div><div class="dname">' + esc(nameOf(item)) + '</div>' +
    '<div class="dmeta">' + esc(item.type) + (item.sub ? ' &middot; ' + esc(item.sub) : '') + '</div></div></div>';
  html += '<dl class="dkv">' +
    '<dt>FormID</dt><dd>' + esc(item.formId) + '</dd>' +
    '<dt>Plugin</dt><dd title="' + esc(item.plugin) + '">' + esc(item.plugin || '-') + '</dd>' +
    '<dt>Value</dt><dd>' + (item.value || 0) + ' credits</dd>' +
    '<dt>Weight</dt><dd>' + (item.weight || 0) + '</dd>' +
    (item.edid ? '<dt>Editor ID</dt><dd title="' + esc(item.edid) + '">' + esc(item.edid) + '</dd>' : '') +
    '</dl>';

  // The other records behind this name. They are real, separately spawnable
  // records - the list collapses them, it does not hide them - so each one is
  // a button that selects it and shows its own stats and preview.
  if ((detail.duplicates || []).length) {
    html += '<div class="sect">Also defined</div><div class="dupes">';
    detail.duplicates.forEach((row) => {
      html += '<button class="dupe" data-dup="' + esc(row.formId) + '" title="Select this record">' +
        '<span class="fid">' + esc(row.formId) + '</span>' +
        '<span class="dplug">' + esc(row.plugin || '-') + '</span>' +
        '<span class="dstat">' + (row.value || 0) + ' cr &middot; ' + (row.weight || 0) + ' wt</span></button>';
    });
    html += '</div>';
  }

  const groups = new Map();
  (detail.mods || []).forEach((option) => {
    if (!groups.has(option.slot)) groups.set(option.slot, []);
    groups.get(option.slot).push(option);
  });
  if (groups.size) {
    html += '<div class="sect">Parts</div>';
    groups.forEach((options, slot) => {
      html += '<div class="optrow"><label>' + esc(slotName(slot)) + '</label><select data-kind="mod" data-slot="' + esc(slot) + '">';
      options.forEach((option) => {
        html += '<option value="' + esc(option.fid) + '"' + (option.selected ? ' selected' : '') + '>' +
          esc(option.label) + (option.default ? ' (default)' : '') + '</option>';
      });
      html += '</select></div>';
    });
  }
  if ((detail.qualities || []).length > 1) {
    html += '<div class="sect">Quality</div><div class="optrow"><label>Tier</label><select data-kind="quality" data-slot="quality">';
    detail.qualities.forEach((option) => {
      html += '<option value="' + esc(option.fid) + '"' + (option.selected ? ' selected' : '') + '>' + esc(option.label) + '</option>';
    });
    html += '</select></div>';
  }
  if ((detail.rarities || []).length) {
    html += '<div class="sect">Rarity</div><div class="optrow"><label>Tier</label><select data-kind="rarity" data-slot="rarity">';
    detail.rarities.forEach((option) => {
      html += '<option value="' + esc(option.fid) + '"' + (option.selected ? ' selected' : '') + '>' + esc(option.label) + '</option>';
    });
    html += '</select></div>';
    if (detail.rarityNote) html += '<p class="note">' + esc(detail.rarityNote) + '</p>';
  }
  if (detail.keywords) html += '<div class="keywords">' + esc(detail.keywords) + '</div>';

  const price = state.free ? 'Free' : ((item.value || 0) + ' credits each');
  html += '<div class="qtyrow"><input id="qty" type="number" min="1" max="999" value="1" title="Quantity">' +
    '<button id="addBtn" class="primary">Add to basket</button></div>' +
    '<p class="note">' + esc(price) + (state.free ? '' : ' &middot; paid from your real balance') + '</p>';
  html += '<p class="note">The parts and rarity above are what the basket line carries.</p>';
  el.detail.innerHTML = html;
}

function slotName(slot) {
  if (slot === 'mag') return 'Magazine';
  if (slot === 'scope') return 'Optic';
  return slot.charAt(0).toUpperCase() + slot.slice(1);
}

/* -------------------------------------------------------------- actions -- */

async function selectItem(formId) {
  if (state.selected !== formId) {
    sfx('select');
    // A different item starts from the INI camera again, exactly like the
    // overlay resetting its orbit when the previewed form changes.
    preview.yaw = 0;
    preview.pitch = 0;
    preview.zoom = 1;
  }
  state.selected = formId;
  // Repaint the highlight, do not rebuild the list. Rebuilding every row on a
  // click replaces the nodes under the cursor, so the second click of a double
  // click lands on a different element and the browser never fires dblclick -
  // which is why double-click stopped adding to the basket. It also keeps the
  // list's scroll position, the same reason the star buttons repaint in place.
  paintSelection();
  requestPreview();
  try {
    const detail = await call(EP.detail, { formId: formId });
    state.detail = detail;
    renderDetail();
  } catch (err) {
    say('Could not read that item: ' + errText(err), 'err');
  }
}

async function addSelected(count) {
  if (!state.detail || !state.detail.ok) { say('Select an item first.', 'err'); return; }
  if (state.basket.length >= MAX_LINES) {
    say('Basket is full (' + MAX_LINES + ' lines) - accept or clear it first.', 'err');
    return;
  }
  try {
    const reply = await call(EP.basket, {
      op: 'add', formId: state.detail.item.formId, count: count || 1, free: state.free,
    });
    applyBasket(reply);
    say(reply && reply.ok ? 'Added to the basket.' : 'Not added: ' + ((reply && reply.error) || 'refused'), reply && reply.ok ? 'ok' : 'err');
  } catch (err) {
    say('Add failed: ' + errText(err), 'err');
  }
}




async function configure(kind, slot, fid) {
  if (!state.detail || !state.detail.ok) return;
  try {
    const reply = await call(EP.configure, { formId: state.detail.item.formId, kind: kind, slot: slot, fid: fid });
    if (reply && reply.detail) {
      state.detail = reply.detail;
      renderDetail();
    }
    if (reply && reply.ok === false) say('That change was refused.', 'err');
  } catch (err) {
    say('Configure failed: ' + errText(err), 'err');
  }
}

/* --------------------------------------------------------------- wiring -- */

el.pluginList.addEventListener('click', (event) => {
  const row = event.target.closest('.prow');
  if (!row) return;
  state.filter.plugin = Number(row.dataset.plugin);
  renderPlugins();
  loadRows(true);
});

el.categories.addEventListener('click', (event) => {
  const button = event.target.closest('button[data-type]');
  if (!button) return;
  state.filter.type = Number(button.dataset.type);
  state.filter.sub = 0;
  renderCategories();
  renderSubs();
  loadRows(true);
});

el.subs.addEventListener('click', (event) => {
  const button = event.target.closest('button[data-sub]');
  if (!button) return;
  state.filter.sub = Number(button.dataset.sub);
  renderSubs();
  loadRows(true);
});

Object.keys(HIDES).forEach((key) => {
  HIDES[key].addEventListener('change', () => {
    state.filter[key] = HIDES[key].checked;
    loadRows(true);
  });
});

if (el.tblhdr) {
  el.tblhdr.addEventListener('click', (event) => {
    const cell = event.target.closest('[data-sort]');
    if (!cell) return;
    const column = Number(cell.dataset.sort);
    // Same column: flip the direction. A new column starts ascending, which
    // is what the overlay's header does.
    if (state.sort.column === column) {
      state.sort.ascending = !state.sort.ascending;
    } else {
      state.sort.column = column;
      state.sort.ascending = true;
    }
    renderSort();
    // Page 0: row 4,000 of the old order is not row 4,000 of the new one.
    loadRows(true);
  });
}

el.rows.addEventListener('click', (event) => {
  // The star is inside the row: its own handler runs and the row is not
  // selected, which is what stops a star click from also highlighting and
  // arming the item.
  const star = event.target.closest('button[data-fav]');
  if (star) {
    event.stopPropagation();
    toggleFavourite(star.dataset.fav);
    return;
  }
  const row = event.target.closest('.trow');
  if (row) selectItem(row.dataset.formid);
});
el.rows.addEventListener('dblclick', (event) => {
  // Two quick star clicks are two toggles, not "select and basket": the star
  // is not the row.
  if (event.target.closest('button[data-fav]')) return;
  const row = event.target.closest('.trow');
  if (!row) return;
  selectItem(row.dataset.formid).then(() => addSelected(Number((document.getElementById('qty') || {}).value) || 1));
});

// Buttons, filter tabs, sort headers and the pager all click the same way, so
// one delegate covers them instead of a call sprinkled through every handler.
// Rows are deliberately excluded: selecting one plays 'select' from
// selectItem, and two sounds for one press is a stutter, not feedback.
document.addEventListener('click', (event) => {
  const hit = event.target.closest('button, .prow, [data-page], [data-sort], [data-type], [data-sub], [data-act], [data-close]');
  if (!hit) return;
  // Removing a basket line has its own sound. It is chosen here, in the
  // delegate, because this runs on capture and fires for every button:
  // a second call from the action handler would stutter.
  if (hit.dataset && hit.dataset.act === 'rm') sfx('remove');
  else sfx('click');
}, true);

// Capture phase: a hover that repaints the row must still sound once.
el.rows.addEventListener('mouseover', (event) => {
  if (event.target.closest('.trow')) sfx('hover');
}, true);

el.pager.addEventListener('click', (event) => {
  const button = event.target.closest('button[data-page]');
  if (!button) return;
  const pages = Math.max(1, Math.ceil(state.total / PAGE));
  state.page = Math.max(0, Math.min(pages - 1, Number(button.dataset.page)));
  loadRows(false);
});

el.detail.addEventListener('change', (event) => {
  const select = event.target.closest('select[data-kind]');
  if (!select) return;
  configure(select.dataset.kind, select.dataset.slot || '', select.value);
});
el.detail.addEventListener('click', (event) => {
  const dupe = event.target.closest('button[data-dup]');
  if (dupe) {
    selectItem(dupe.dataset.dup);
    return;
  }
  if (event.target.id !== 'addBtn') return;
  const qty = document.getElementById('qty');
  addSelected(Number(qty && qty.value) || 1);
});

el.basketItems.addEventListener('click', (event) => {
  const button = event.target.closest('button[data-act]');
  if (!button) return;
  const index = Number(button.dataset.i);
  const line = state.basket[index];
  if (!line) return;
  if (button.dataset.act === 'rm') basketOp('remove', { index: index });
  else if (button.dataset.act === 'inc') basketOp('set', { formId: line.formId, count: Math.min(999, line.count + 1) });
  else if (button.dataset.act === 'dec') basketOp('set', { formId: line.formId, count: Math.max(1, line.count - 1) });
});

el.acceptBtn.addEventListener('click', acceptBasket);
el.clearBtn.addEventListener('click', () => { basketOp('clear'); say('Basket cleared.'); });
el.free.addEventListener('change', () => {
  state.free = el.free.checked;
  renderBasket();
  renderDetail();
  loadRows(false);
});

let searchTimer = 0;
function queueSearch() {
  clearTimeout(searchTimer);
  searchTimer = setTimeout(() => {
    state.search = el.search.value.trim();
    loadRows(true);
  }, 300);
}
el.search.addEventListener('input', queueSearch);
el.search.addEventListener('keydown', (event) => {
  if (event.key !== 'Enter') return;
  clearTimeout(searchTimer);
  state.search = el.search.value.trim();
  loadRows(true);
});
el.searchBtn.addEventListener('click', () => {
  clearTimeout(searchTimer);
  state.search = el.search.value.trim();
  loadRows(true);
});

el.refresh.addEventListener('click', async () => {
  say('Rebuilding the catalog...');
  try {
    applyCatalog(await call(EP.refresh, { free: state.free }));
    await loadRows(true);
    say('Refreshed: ' + state.plugins.length + ' plugins.', 'ok');
  } catch (err) {
    say('Refresh failed: ' + errText(err), 'err');
  }
});

/* keyboard: arrows walk the page, Enter adds the selected item */
document.addEventListener('keydown', (event) => {
  const typing = document.activeElement && /^(INPUT|SELECT|TEXTAREA)$/.test(document.activeElement.tagName);
  if (typing) return;
  if (event.key === 'ArrowDown' || event.key === 'ArrowUp') {
    if (!state.rows.length) return;
    event.preventDefault();
    let index = state.rows.findIndex((row) => row.formId === state.selected);
    index = event.key === 'ArrowDown' ? Math.min(state.rows.length - 1, index + 1) : Math.max(0, index - 1);
    selectItem(state.rows[index].formId);
    const node = el.rows.children[index];
    if (node && node.scrollIntoView) node.scrollIntoView({ block: 'nearest' });
  } else if (event.key === 'Enter' && state.selected) {
    event.preventDefault();
    const qty = document.getElementById('qty');
    addSelected(Number(qty && qty.value) || 1);
  }
});

/* gamepad: OSF UI publishes raw sticks/buttons when a view asks for them.
 * Left stick / d-pad move a cursor, A clicks, X adds, LB/RB page, B closes,
 * Start accepts. Mouse movement hides the cursor. */
const pad = { x: innerWidth / 2, y: innerHeight / 2, node: null, visible: false, vx: 0, vy: 0, svx: 0, svy: 0, sry: 0, dpad: {}, raf: 0, last: 0 };

function padEnsure() {
  if (!pad.node) {
    pad.node = document.createElement('div');
    pad.node.id = 'padCursor';
    pad.node.style.cssText = 'position:fixed;width:14px;height:14px;margin:-7px 0 0 -7px;border-radius:50%;' +
      'border:2px solid #ffd166;box-shadow:0 0 10px #ffd16688;pointer-events:none;z-index:99;display:none';
    document.body.appendChild(pad.node);
  }
  pad.node.style.left = pad.x + 'px';
  pad.node.style.top = pad.y + 'px';
  pad.node.style.display = 'block';
  pad.visible = true;
}

function padMove(dx, dy) {
  padEnsure();
  pad.x = Math.max(4, Math.min(innerWidth - 4, pad.x + dx));
  pad.y = Math.max(4, Math.min(innerHeight - 4, pad.y + dy));
  pad.node.style.left = pad.x + 'px';
  pad.node.style.top = pad.y + 'px';
}

function padHide() {
  pad.svx = pad.svy = pad.sry = 0;
  if (pad.visible && pad.node) { pad.node.style.display = 'none'; pad.visible = false; }
}

function padUnder() {
  return document.elementFromPoint(pad.x, pad.y);
}

function padClick() {
  const target = padUnder();
  if (!target) return;
  const hit = target.closest('.trow,.prow,.rail button,.chips button,button,select,label') || target;
  hit.dispatchEvent(new MouseEvent('click', { bubbles: true, cancelable: true, view: window }));
}

function padAdd() {
  const target = padUnder();
  const row = target && target.closest('.trow');
  if (!row) { padClick(); return; }
  selectItem(row.dataset.formid).then(() => addSelected(Number((document.getElementById('qty') || {}).value) || 1));
}

function padPage(direction) {
  const pages = Math.max(1, Math.ceil(state.total / PAGE));
  const next = Math.max(0, Math.min(pages - 1, state.page + direction));
  if (next !== state.page) { state.page = next; loadRows(false); }
}

function padScroll(pixels) {
  const target = padUnder();
  if (!target) return;
  let node = target;
  while (node) {
    const style = getComputedStyle(node);
    if (/(auto|scroll)/.test(style.overflowY) && node.scrollHeight > node.clientHeight + 2) {
      node.scrollBy(0, -pixels);
      return;
    }
    node = node.parentElement;
  }
}

function padFrame(timestamp) {
  pad.raf = 0;
  const dt = Math.min(0.05, pad.last ? (timestamp - pad.last) / 1000 : 0.016);
  pad.last = timestamp;
  const ease = Math.min(1, dt * 10);
  pad.vx += (pad.svx - pad.vx) * ease;
  pad.vy += (pad.svy - pad.vy) * ease;
  const step = 520 * dt;
  const dx = pad.vx * dt + (pad.dpad.l ? -step : 0) + (pad.dpad.r ? step : 0);
  const dy = pad.vy * dt + (pad.dpad.u ? -step : 0) + (pad.dpad.d ? step : 0);
  if (Math.abs(dx) > 0.01 || Math.abs(dy) > 0.01) padMove(dx, dy);
  if (Math.abs(pad.sry) > 1) padScroll(pad.sry * dt);
  if (Math.abs(pad.vx) > 0.5 || Math.abs(pad.vy) > 0.5 || Math.abs(pad.sry) > 1 || pad.dpad.l || pad.dpad.r || pad.dpad.u || pad.dpad.d) {
    pad.raf = requestAnimationFrame(padFrame);
  } else {
    pad.vx = pad.vy = pad.sry = 0;
    pad.last = 0;
  }
}

function padKick() {
  if (!pad.raf) { pad.last = 0; pad.raf = requestAnimationFrame(padFrame); }
}

function padButton(id, down) {
  if (id === 0x0001 || id === 0x0002 || id === 0x0004 || id === 0x0008) {
    const key = { 1: 'u', 2: 'd', 4: 'l', 8: 'r' }[id];
    if (down) pad.dpad[key] = 1; else delete pad.dpad[key];
    if (down) padKick();
    return;
  }
  if (!down) return;
  if (id === 0x1000) padClick();
  else if (id === 0x4000) padAdd();
  else if (id === 0x2000) send('close');
  else if (id === 0x0100) padPage(-1);
  else if (id === 0x0200) padPage(1);
  else if (id === 0x0010) acceptBasket();
  else if (id === 0x0020) basketOp('clear');
}

function padStick(axes) {
  const dead = 0.25;
  const curve = (value) => Math.sign(value) * Math.pow(Math.abs(value), 1.4);
  pad.svx = curve(axes.lx || 0) * 2400;
  pad.svy = -curve(axes.ly || 0) * 2400;
  pad.sry = Math.abs(axes.ry || 0) > dead ? curve(axes.ry) * 2000 : 0;
  if (Math.abs(axes.lx || 0) > dead || Math.abs(axes.ly || 0) > dead) padEnsure();
  padKick();
}

function wireGamepad() {
  const g = bridge();
  if (!g || typeof g.on !== 'function') return;
  send('osfui.gamepadRaw', { raw: true });
  document.addEventListener('pointermove', padHide, { passive: true });
  g.on('ui.gamepad', (event) => {
    if (!event || typeof event !== 'object') return;
    const button = event.button || event.btn || null;
    const id = button ? (button.id !== undefined ? button.id : button.idCode) : (typeof event.id === 'number' ? event.id : undefined);
    if (id !== undefined && id !== null) padButton(Number(id) & 0xffff, !!(button ? button.down : event.down));
    const axes = event.axes || event.stick || (typeof event.lx === 'number' ? event : null);
    if (axes) padStick(axes);
  });
}

/* -------------------------------------------------------------- preview -- */

/* The native side renders the selected item offscreen and answers with the PNG
 * itself as a data URL: OSF UI serves a view from a snapshot taken at startup,
 * so a file the plugin writes beside the page would never load. This panel is
 * just an <img>.
 *
 * A drag streams frames: every pointer sample asks for its camera, one frame is
 * in flight at a time, and samples that arrive meanwhile are folded into the
 * newest camera rather than queued one by one. The native side quantises to
 * whole degrees, and cameraKey() drops a sample that rounds to the frame already
 * asked for, so a slow nudge does not spend a data URL per pixel and a fast
 * drag still moves through several degrees per frame. */

let lastPreviewError = '';

function previewFailed(message) {
  pv.img.hidden = true;
  pv.img.removeAttribute('src');
  pv.hint.hidden = true;
  pv.empty.hidden = false;
  pv.empty.textContent = message;
  // A renderer that fails in game leaves no trace either side of the bridge,
  // so the reason is mirrored into the plugin log once per distinct message.
  if (message && message !== lastPreviewError) {
    lastPreviewError = message;
    reportToNative('preview: ' + message);
  }
}

function previewShown(reply) {
  const src = reply.data || '';
  if (!src) {
    previewFailed('No preview for this item.');
    return;
  }
  if (src !== pv.img.getAttribute('src')) pv.img.src = src;
  pv.img.hidden = false;
  pv.empty.hidden = true;
  pv.hint.hidden = false;
  lastPreviewError = '';
}

/* The camera the plugin would render for, at its own 1-degree quantisation.
 * A drag fires a pointermove per mouse sample, and most of those land on the
 * same degree; asking twice for it would cost a full data URL each time, so a
 * repeated key is skipped and the newest key is what gets sent. */
function cameraKey() {
  if (!state.selected) return '';
  const step = (value) => Math.round(value);
  return state.selected + '|' + step(preview.yaw) + '|' + step(preview.pitch) + '|' + step(preview.zoom * 100);
}

async function requestPreview(force) {
  if (!state.selected) {
    previewFailed('Select an item.');
    return;
  }
  const key = cameraKey();
  if (!force && key === preview.asked) return;
  if (preview.busy) {
    preview.queued = true;
    return;
  }
  preview.asked = key;
  preview.busy = true;
  const token = ++preview.token;
  const formId = state.selected;
  try {
    // A frame is rendered on the game thread, so give it longer than a query.
    const reply = await call(EP.preview, {
      formId: formId, yaw: preview.yaw, pitch: preview.pitch, zoom: preview.zoom,
    }, 20000);
    if (token !== preview.token) return;  // a newer camera already answered
    if (!reply || reply.ok === false) {
      // Forget the camera so the next drag sample asks again rather than being
      // suppressed as a repeat of a frame that never arrived.
      preview.asked = '';
      previewFailed((reply && reply.error) || 'No preview for this item.');
      return;
    }
    previewShown(reply);
  } catch (err) {
    if (token === preview.token) {
      preview.asked = '';
      previewFailed('Preview failed: ' + errText(err));
    }
  } finally {
    preview.busy = false;
    if (preview.queued) {
      preview.queued = false;
      requestPreview();
    }
  }
}

function schedulePreview(delay, force) {
  clearTimeout(preview.timer);
  preview.timer = setTimeout(() => requestPreview(force), delay || 0);
}

pv.img.addEventListener('error', () => {
  previewFailed('The preview could not be drawn.');
});

/* OSF UI forwards the game's mouse into the WebView2 host over CDP, and a
 * press can reach the page as a pointer event, as a plain mouse event, or as
 * both. Only one family is allowed to own a drag - a trusted pointerdown is
 * followed by a mousedown for the same press, and counting the movement twice
 * would rotate at double speed - so the first press latches the family and the
 * other one stands down until the drag ends. */

let dragFamily = '';

function beginDrag(x, y) {
  if (pv.img.hidden) return;
  preview.drag = { x: x, y: y };
  preview.moves = 0;
}

function moveDrag(x, y) {
  if (!preview.drag) return;
  const dx = x - preview.drag.x;
  const dy = y - preview.drag.y;
  preview.drag = { x: x, y: y };
  preview.yaw += dx * 0.6;
  preview.pitch = Math.max(-85, Math.min(85, preview.pitch + dy * 0.5));
  preview.moves += 1;
  // No debounce while the pointer is down: the single-flight queue in
  // requestPreview() keeps one frame in the plugin and folds every sample that
  // arrives meanwhile into the newest camera, so the drag streams frames at
  // whatever rate the renderer manages instead of showing one still after the
  // pointer stops.
  requestPreview();
}

function endDrag() {
  if (!preview.drag) return;
  preview.drag = null;
  dragFamily = '';
  // One line per drag in the plugin log: a view that never reaches this point
  // is a view that never received the press, which is worth knowing when the
  // in-game pointer path changes.
  if (preview.moves) {
    reportToNative('preview drag: yaw=' + Math.round(preview.yaw) +
      ' pitch=' + Math.round(preview.pitch) + ' samples=' + preview.moves);
    preview.moves = 0;
  }
  // The last sample can be skipped as a repeat of the frame already in flight;
  // force one final frame so the pointer-up camera is what stays on screen.
  schedulePreview(0, true);
}

pv.box.addEventListener('pointerdown', (event) => {
  if (pv.img.hidden) return;
  event.preventDefault();
  dragFamily = 'pointer';
  // Capture keeps the drag alive when the pointer leaves the panel. It can
  // legitimately fail (the pointer is already gone), and the drag must not
  // depend on it, so a refusal just means no capture.
  try { pv.box.setPointerCapture(event.pointerId); } catch (err) { /* no capture */ }
  beginDrag(event.clientX, event.clientY);
});

pv.box.addEventListener('pointermove', (event) => {
  if (dragFamily === 'mouse') return;
  moveDrag(event.clientX, event.clientY);
});

// Mouse fallback: a host that forwards presses as mouse events only would
// otherwise leave the camera stuck, because a synthetic mousedown produces no
// pointer events at all.
pv.box.addEventListener('mousedown', (event) => {
  if (preview.drag) return;
  event.preventDefault();
  dragFamily = 'mouse';
  beginDrag(event.clientX, event.clientY);
});

window.addEventListener('mousemove', (event) => {
  if (dragFamily !== 'mouse') return;
  moveDrag(event.clientX, event.clientY);
}, true);

// Released outside the panel (or outside the pop-out) still ends the drag, so
// the camera can never be left rotating under a hovering cursor.
window.addEventListener('pointerup', endDrag, true);
window.addEventListener('pointercancel', endDrag, true);
window.addEventListener('mouseup', endDrag, true);
window.addEventListener('blur', endDrag);

pv.box.addEventListener('wheel', (event) => {
  if (pv.img.hidden) return;
  event.preventDefault();
  preview.zoom = Math.max(0.4, Math.min(3, preview.zoom * (event.deltaY > 0 ? 1.1 : 1 / 1.1)));
  schedulePreview(40);
}, { passive: false });

pv.box.addEventListener('dblclick', () => {
  preview.yaw = 0;
  preview.pitch = 0;
  preview.zoom = 1;
  schedulePreview(0);
});

/* -------------------------------------------------------- pop-out frame -- */

/* The float is a shell around #preview itself rather than a second preview:
 * re-parenting the node keeps the drag offset, the zoom and the last frame, and
 * the plugin keeps answering into the same element because none of its
 * listeners moved. The dock button puts it back where the markup had it. */

const pvFloat = document.getElementById('previewFloat');
const pvFloatHead = document.getElementById('previewFloatHead');
const pvFloatBody = document.getElementById('previewFloatBody');
const pvFloatName = document.getElementById('previewFloatName');
const pvPopBtn = document.getElementById('previewPop');
const pvDockBtn = document.getElementById('previewDock');
const pvHome = pv.box.parentNode;
const pvHomeAnchor = pv.box.nextElementSibling;

let floatDrag = null;

function previewFloating() {
  return !pvFloat.hidden;
}

function clampFloat() {
  if (!previewFloating()) return;
  const rect = pvFloat.getBoundingClientRect();
  // A window dragged past the edge (or a shop view that shrank) comes back.
  const left = Math.min(Math.max(0, rect.left), Math.max(0, window.innerWidth - rect.width));
  const top = Math.min(Math.max(0, rect.top), Math.max(0, window.innerHeight - rect.height));
  pvFloat.style.left = left + 'px';
  pvFloat.style.top = top + 'px';
}

function popPreview() {
  if (previewFloating()) return;
  pvFloatBody.appendChild(pv.box);
  pvFloat.hidden = false;
  pvPopBtn.textContent = 'Dock';
  clampFloat();
  pvPopBtn.blur();
}

function dockPreview() {
  if (!previewFloating()) return;
  pvFloat.hidden = true;
  const anchor = pvHomeAnchor && pvHomeAnchor.parentNode === pvHome ? pvHomeAnchor : null;
  pvHome.insertBefore(pv.box, anchor);
  pvPopBtn.textContent = 'Pop out';
}

pvPopBtn.addEventListener('click', popPreview);
pvDockBtn.addEventListener('click', dockPreview);

pvFloatHead.addEventListener('pointerdown', (event) => {
  if (event.target.closest('button')) return;
  event.preventDefault();
  const rect = pvFloat.getBoundingClientRect();
  try { pvFloatHead.setPointerCapture(event.pointerId); } catch (err) { /* no capture */ }
  floatDrag = { x: event.clientX, y: event.clientY, left: rect.left, top: rect.top };
  pvFloatHead.classList.add('grabbing');
});

pvFloatHead.addEventListener('pointermove', (event) => {
  if (!floatDrag) return;
  pvFloat.style.left = (floatDrag.left + event.clientX - floatDrag.x) + 'px';
  pvFloat.style.top = (floatDrag.top + event.clientY - floatDrag.y) + 'px';
});

function endFloatDrag() {
  if (!floatDrag) return;
  floatDrag = null;
  pvFloatHead.classList.remove('grabbing');
  clampFloat();
}

pvFloatHead.addEventListener('pointerup', endFloatDrag);
[pvFloatHead, pvFloat].forEach((node) => node.addEventListener('pointercancel', endFloatDrag));
window.addEventListener('resize', clampFloat);

/* --------------------------------------------------------------- themes -- */

function applyTheme(row) {
  if (!row) return;
  const root = document.documentElement.style;
  const mix = (color, percent, onto) => 'color-mix(in srgb, ' + color + ' ' + percent + '%, ' + (onto || 'transparent') + ')';
  root.setProperty('--ink', row.ink);
  root.setProperty('--muted', row.muted);
  root.setProperty('--accent', row.accent);
  root.setProperty('--accent-dim', row.border);
  root.setProperty('--gold', row.gold);
  root.setProperty('--gold-dim', mix(row.gold, 45, '#000'));
  root.setProperty('--panel', mix(row.panel, 90));
  root.setProperty('--panel-alt', mix(row.panelAlt, 93));
  root.setProperty('--line', row.border);
  root.setProperty('--hover', mix(row.accent, 24, '#000'));
  applyBackdrop(row.suffix);
}

/* Where the backdrop art comes from, in one place. The three shipped themes
 * keep the view's own art: those files are already beside the page, so no
 * bytes cross the bridge. A player's own image is not there - OSF serves a
 * view from a snapshot taken at startup, so a file dropped beside the page is
 * never served - and the plugin hands it over as a data URL instead, which is
 * also how the item previews travel. The shared art is the last resort, so a
 * backdrop whose file is gone degrades to the old look rather than to an
 * empty background. */
const backdropArt = new Map();
const VIEW_BACKDROPS = ['starfield', 'hell', 'heaven'];

/* One custom property feeds the single backdrop layer. Setting the property
 * instead of the element's background keeps the default and every theme on the
 * same element and the same `contain` rule. */
function setBackdrop(url) {
  document.documentElement.style.setProperty('--bg-art', 'url("' + url + '")');
  scheduleBackdropDiag();
}

/* One framing line per page load, written to the plugin log. A report of a
 * zoomed backdrop cannot be told apart from a stale cached stylesheet by
 * looking at the screen, so this records what the running page actually
 * resolved: the computed background-size, the URL it loaded, that image's
 * natural size and the viewport it was fitted into. */
let backdropDiagDone = false;
let backdropDiagFallback = false;
let backdropDiagTimer = 0;

function scheduleBackdropDiag() {
  clearTimeout(backdropDiagTimer);
  backdropDiagTimer = setTimeout(reportBackdropDiag, 300);
}

function reportBackdropDiag(fallback) {
  if (fallback) {
    if (backdropDiagFallback || backdropDiagDone) return;
    backdropDiagFallback = true;
  } else {
    if (backdropDiagDone) return;
    backdropDiagDone = true;
  }
  const layer = document.getElementById('bgimg');
  if (!layer) {
    reportToNative('backdrop diag: #bgimg missing');
    return;
  }
  const style = getComputedStyle(layer);
  const rect = layer.getBoundingClientRect();
  const match = /^url\(["']?([\s\S]*?)["']?\)$/.exec(String(style.backgroundImage || ''));
  const url = match ? match[1] : '';
  const shown = url.slice(0, 5) === 'data:' ? 'data url (' + url.length + ' chars)' : (url || 'none');
  const base = 'backdrop diag' + (fallback ? ' (fallback)' : '') + ': size=' + style.backgroundSize +
    '; position=' + style.backgroundPosition +
    '; layer=' + Math.round(rect.width) + 'x' + Math.round(rect.height) +
    '; viewport=' + innerWidth + 'x' + innerHeight +
    '; client=' + document.documentElement.clientWidth + 'x' + document.documentElement.clientHeight +
    '; dpr=' + devicePixelRatio +
    '; image=' + shown;
  if (!url) {
    reportToNative(base + '; natural=none');
    return;
  }
  const probe = new Image();
  probe.onload = () => reportToNative(base + '; natural=' + probe.naturalWidth + 'x' + probe.naturalHeight);
  probe.onerror = () => reportToNative(base + '; natural=load failed');
  probe.src = url;
}

async function applyBackdrop(suffix) {
  if (!suffix) {
    setBackdrop('assets/space.jpg?v=' + ASSET_VERSION);
    return;
  }
  if (backdropArt.has(suffix)) {
    setBackdrop(backdropArt.get(suffix));
    return;
  }
  // Order matters: a shipped name is a file of the view, a dropped one can only
  // come from the plugin, so asking the wrong source first costs either a 404
  // or a few hundred kilobytes of base64.
  const sources = VIEW_BACKDROPS.indexOf(suffix) >= 0 ? [fromView, pluginBackdrop] : [pluginBackdrop, fromView];
  for (const source of sources) {
    const art = await source(suffix);
    if (art) {
      setBackdrop(art);
      return;
    }
  }
  setBackdrop('assets/space.jpg?v=' + ASSET_VERSION);
}

async function fromView(suffix) {
  return (await probeImage('assets/space_' + suffix + '.jpg?v=' + ASSET_VERSION)) ||
    (await probeImage('assets/space_' + suffix + '.png?v=' + ASSET_VERSION));
}

function probeImage(src) {
  return new Promise((resolve) => {
    const probe = new Image();
    probe.onload = () => resolve(probe.src);
    probe.onerror = () => resolve('');
    probe.src = src;
  });
}


/* The plugin's own copy of the art, which is the only route a dropped-in image
 * has. Cached per name: every colour tweak re-applies the theme, and a backdrop
 * is hundreds of kilobytes of base64. A miss is not cached, so an image added
 * during a session is picked up on the next theme switch. */
async function pluginBackdrop(name) {
  try {
    const reply = await call(EP.backdrop, { op: 'art', name: name }, 20000);
    if (reply && reply.ok && reply.data) {
      backdropArt.set(name, reply.data);
      return reply.data;
    }
    if (reply && reply.error) reportToNative('backdrop ' + name + ' unavailable: ' + reply.error);
  } catch (err) {
    reportToNative('backdrop ' + name + ' got no answer: ' + errText(err));
  }
  return '';
}

function renderThemeList() {
  const host = document.getElementById('themeList');
  let html = '';
  themes.rows.forEach((row) => {
    const isActive = row.index === themes.active;
    const locked = row.premium && !row.unlocked;
    html += '<button class="theme-card' + (isActive ? ' on' : '') + '" data-theme="' + row.index + '"' +
      (locked ? ' disabled' : '') + ' title="' + esc(locked ? 'Locked: ' + row.progress : row.name) + '">' +
      '<span class="swatches"><span style="background:' + row.accent + '"></span>' +
      '<span style="background:' + row.gold + '"></span>' +
      '<span style="background:' + row.panelAlt + '"></span></span>' +
      '<span class="tname">' + esc(row.name) + '</span>' +
      (locked ? '<span class="tlock">Locked &middot; ' + esc(row.progress) + '</span>'
              : '<span class="tunlocked">' + (isActive ? 'In use' : (row.premium ? 'Earned' : 'Ready')) + '</span>') +
      '</button>';
  });
  host.innerHTML = html;
  // Only the custom theme has anything to edit, and only while it is in use.
  const activeRow = themes.rows.find((row) => row.index === themes.active);
  document.getElementById('customThemeRow').hidden = !(activeRow && activeRow.custom);
}

async function loadThemes() {
  try {
    const reply = await call(EP.themes, {});
    if (!reply || reply.ok === false) return;
    themes.rows = reply.themes || [];
    themes.active = Number(reply.active) || 0;
    const active = themes.rows.find((row) => row.index === themes.active);
    applyTheme(active);
    renderThemeList();
  } catch (err) {
    say('Themes unavailable: ' + errText(err), 'err');
  }
}

document.getElementById('themeList').addEventListener('click', async (event) => {
  const card = event.target.closest('button[data-theme]');
  if (!card || card.disabled) return;
  const index = Number(card.dataset.theme);
  try {
    const reply = await call(EP.themepick, { index: index });
    if (reply && reply.themes) {
      themes.rows = reply.themes;
      themes.active = Number(reply.active);
      applyTheme(themes.rows.find((row) => row.index === themes.active));
      renderThemeList();
    }
    if (!reply || reply.ok === false) say((reply && reply.error) || 'That theme is still locked.', 'err');
    else say('Theme changed.', 'ok');
  } catch (err) {
    say('Theme change failed: ' + errText(err), 'err');
  }
});

/* ------------------------------------------------- custom theme editor -- */

/* The nine colours this page actually renders. The overlay's CUSTOMISE window
 * owns the rest of the palette (table rows, separators, hover states) and the
 * pane layout, but both write the same [custom] / [customcolours] keys, so a
 * theme edited here opens in the overlay unchanged and vice versa. */
const CT_KEYS = ['accent', 'accentBright', 'gold', 'ink', 'muted', 'border', 'panel', 'panelAlt', 'windowBg'];
const CT_LABELS = {
  accent: 'Accent', accentBright: 'Accent bright', gold: 'Gold',
  ink: 'Text', muted: 'Muted text', border: 'Border',
  panel: 'Panel', panelAlt: 'Panel alt', windowBg: 'Window',
};

/* The Art list is what this install actually has, not what the build was
 * compiled with: the plugin answers with the space_<name> files beside it, and
 * the three shipped names are added because their art ships in the view. */
async function fillBackdropOptions() {
  const select = document.getElementById('ctBackdrop');
  let names = [];
  try {
    const reply = await call(EP.backdrop, { op: 'list' });
    if (reply && reply.ok && Array.isArray(reply.backdrops)) {
      names = reply.backdrops.map((row) => row.name).filter(Boolean);
    }
  } catch (err) {
    reportToNative('backdrop list unavailable: ' + errText(err));
  }
  ['starfield', 'hell', 'heaven'].forEach((name) => {
    if (names.indexOf(name) < 0) names.push(name);
  });
  names.sort();
  let html = '<option value="">(shared backdrop)</option>';
  names.forEach((name) => {
    html += '<option value="' + esc(name) + '">' + esc(name) + '</option>';
  });
  const saved = custom.suffix || '';
  if (saved && names.indexOf(saved) < 0) {
    // Still offered: it is this theme's saved choice, and dropping the option
    // would silently reset it on the next Save.
    html += '<option value="' + esc(saved) + '">' + esc(saved) + ' (missing)</option>';
  }
  select.innerHTML = html;
}
function renderCustomEditor() {
  document.getElementById('ctName').value = custom.name || 'Custom';
  document.getElementById('ctBackdrop').value = custom.suffix || '';
  let html = '';
  CT_KEYS.forEach((key) => {
    html += '<label class="ctrow"><span>' + CT_LABELS[key] + '</span>' +
      '<input type="color" data-key="' + key + '" value="' + (custom.colours[key] || '#808080') + '"></label>';
  });
  document.getElementById('ctColours').innerHTML = html;
}

function readCustomEditor() {
  custom.name = (document.getElementById('ctName').value || '').trim() || 'Custom';
  custom.suffix = document.getElementById('ctBackdrop').value;
  document.querySelectorAll('#ctColours input[data-key]').forEach((input) => {
    custom.colours[input.dataset.key] = input.value;
  });
}

async function loadCustomTheme() {
  try {
    const reply = await call(EP.customtheme, {});
    if (!reply || reply.ok === false) return;
    custom.name = reply.name || 'Custom';
    custom.suffix = reply.suffix || '';
    custom.colours = reply.colours || {};
    // The list first: renderCustomEditor sets the select's value, and a
    // value with no matching option is dropped.
    await fillBackdropOptions();
    renderCustomEditor();
  } catch (err) {
    say('Custom theme unavailable: ' + errText(err), 'err');
  }
}

/* A colour change redraws the shop behind the dialog, so the palette is judged
 * by looking at it. Nothing is written until Save. */
function previewCustomTheme() {
  const row = themes.rows.find((r) => r.index === themes.active);
  if (!row || !row.custom) return;
  applyTheme(Object.assign({}, row, {
    accent: custom.colours.accent,
    gold: custom.colours.gold,
    panel: custom.colours.panel,
    panelAlt: custom.colours.panelAlt,
    ink: custom.colours.ink,
    muted: custom.colours.muted,
    border: custom.colours.border,
    suffix: custom.suffix,
  }));
}

async function saveCustomTheme() {
  readCustomEditor();
  const payload = { op: 'set', name: custom.name, suffix: custom.suffix };
  CT_KEYS.forEach((key) => { if (custom.colours[key]) payload[key] = custom.colours[key]; });
  try {
    const reply = await call(EP.customtheme, payload);
    if (!reply || reply.ok === false) { say('Theme not saved.', 'err'); return; }
    custom.name = reply.name || custom.name;
    custom.suffix = reply.suffix || '';
    custom.colours = reply.colours || custom.colours;
    await loadThemes();
    closePopups();
    say('Custom theme saved.', 'ok');
  } catch (err) {
    say('Save failed: ' + errText(err), 'err');
  }
}

document.getElementById('customThemeBtn').addEventListener('click', () => {
  openPopup('custom');
  loadCustomTheme();
});
document.getElementById('ctBackdrop').addEventListener('change', () => {
  readCustomEditor();
  previewCustomTheme();
});
document.getElementById('ctColours').addEventListener('input', () => {
  readCustomEditor();
  previewCustomTheme();
});
document.getElementById('ctSave').addEventListener('click', saveCustomTheme);
document.getElementById('ctClose').addEventListener('click', closePopups);

/* Closing without Save puts the saved palette back: the preview applies straight
 * to the page, so dropping the dialog must undo it, or a colour that was never
 * saved would look as if it had been. */
function revertCustomTheme() {
  if (!popups.custom.hidden) loadThemes();
}
document.addEventListener('click', (event) => {
  if (event.target.closest && event.target.closest('[data-close="customPop"]')) revertCustomTheme();
});
popups.custom.addEventListener('click', (event) => {
  if (event.target === popups.custom) revertCustomTheme();
});
document.addEventListener('keydown', (event) => {
  if (event.key === 'Escape') revertCustomTheme();
});

/* --------------------------------------------------------------- popups -- */

function openPopup(name) {
  Object.keys(popups).forEach((key) => { popups[key].hidden = key !== name; });
  if (name === 'audio') loadAudio();
  if (name === 'theme') loadThemes();
}

function closePopups() {
  Object.keys(popups).forEach((key) => { popups[key].hidden = true; });
}

el.audioBtn.addEventListener('click', () => openPopup('audio'));
el.themeBtn.addEventListener('click', () => openPopup('theme'));
el.hideBtn.addEventListener('click', () => openPopup('hide'));
el.favouritesOnly.addEventListener('change', () => {
  state.favouritesOnly = el.favouritesOnly.checked;
  // The plugin builds the starred list (every plugin, every category) and the
  // page just shows what comes back; the count and the empty message follow.
  loadRows(true);
});
document.addEventListener('click', (event) => {
  const closer = event.target.closest('[data-close]');
  if (closer) closePopups();
  if (event.target.classList && event.target.classList.contains('popup')) closePopups();
});
document.addEventListener('keydown', (event) => {
  if (event.key === 'Escape') closePopups();
  // The bound open key closes the shop too, as the ImGui build does. OSF's
  // hotkey service never delivers it while the view is up, so ask the plugin
  // to close the view through OSF.
  if (state.openKey && event.key === state.openKey) send(MOD + '.close', {});
});

/* ----------------------------------------------------------------- boot -- */

function start() {
  if (state.booted) return;
  state.booted = true;
  hookMessages();
  hookReplies();
  wireGamepad();
  watchViewState();
  // Uncaught page errors go to the plugin log too; a blank shop with no reason
  // in either log is what sent this hunt to the OSF bridge in the first place.
  window.addEventListener('error', (event) => {
    reportToNative('script error: ' + (event && event.message ? event.message : 'unknown'));
  });
  window.addEventListener('unhandledrejection', (event) => {
    const reason = event && event.reason;
    reportToNative('unhandled rejection: ' + (reason && reason.message ? reason.message : reason));
  });
  say('Connecting...');
  renderSort();
  loadCatalog();
  loadAudio();
  loadThemes();
  // If the theme reply never arrives, the stylesheet's default art is still on
  // screen; log that framing too instead of leaving the hunt with no line.
  setTimeout(() => reportBackdropDiag(true), 2000);
  // The page can be loaded before the save has finished loading, so keep the
  // retry alive until plugins answer. Interaction also re-syncs, because OSF UI
  // keeps this document alive between opens.
  setInterval(() => { if (!state.plugins.length) loadCatalog(); }, 15000);
  document.addEventListener('focus', () => { if (Date.now() - (state.lastSync || 0) > 3000) { state.lastSync = Date.now(); loadCatalog(); } });
  document.addEventListener('pointerdown', () => { state.lastSync = Date.now(); });
}

(function waitForBridge() {
  let tries = 0;
  const timer = setInterval(() => {
    if (bridgeReady()) {
      clearInterval(timer);
      start();
    } else if (++tries > 120) {
      clearInterval(timer);
      say('OSF UI bridge never became available. Check that OSF UI is installed and enabled.', 'err');
    }
  }, 250);
})();
