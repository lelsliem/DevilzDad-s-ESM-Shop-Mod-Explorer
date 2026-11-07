"""Serve the OSF shop page with a mocked OSF UI bridge.

The page's only integration point is window.osfui. This harness implements the
same request names and payload shapes the native plugin answers with, so the
catalog, detail, basket and accept paths can be exercised in a browser without
launching Starfield. It is a development aid, not shipped in the mod.

    python tests/mock_server.py [port]

Then open http://127.0.0.1:<port>/ in any browser.
"""

import http.server
import os
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..",
                    "SFSE", "Plugins", "OSFUI", "views", "devilzdad.osfshop", "shop")

MOCK_JS = r"""
(function () {
  const CATALOG = [
    {index: 0, name: 'Weapons', subs: ['All Weapons','Pistols','Rifles','Shotguns','Snipers','Heavy','Melee','Thrown','Other']},
    {index: 1, name: 'Armor', subs: ['All Armor','Spacesuits','Helmets','Hats / Caps','Accessories','Backpacks','Apparel','Other']},
    {index: 2, name: 'Ammo', subs: ['All Ammo','Ballistic','Energy / Particle','Missiles','Mines / Grenades','Ship / Creature','Other']},
    {index: 3, name: 'Misc', subs: ['All Misc','Flora','Toys','Cards','Resources','Containers','Décor','Lab / Samples','Creature Parts','Tech / Parts','Data Slates','Food / Drink','Artifacts','Animal Parts','Other']},
    {index: 4, name: 'Aid', subs: ['All Aid','Food / Drink','Medical','Chems','Other']},
    {index: 5, name: 'Books', subs: ['All Books','Slates','Notes / Reports','Logs / Journals','Guides / Books','Misc / Other']},
    {index: 6, name: 'Keys', subs: ['All Keys','Keycards / Access','Keys','Codes / Passcodes','Other']},
    {index: 8, name: 'All Types', subs: ['All Types']},
  ];
  const PLUGINS = [
    {name: 'Starfield.esm', kind: 'ESM'},
    {name: 'ShatteredSpace.esm', kind: 'ESM'},
    {name: 'MyGunPack.esp', kind: 'ESP'},
    {name: 'SmallLight.esl', kind: 'ESL'},
  ];
  const ITEMS = [
    {formId:'0026A1B2', name:'Razorback "Prototype"', edid:'WPN_Razorback_Prototype', typeIndex:0, type:'Weapons', subIndex:2, sub:'Rifles', plugin:'MyGunPack.esp', value:5432, weight:7.3},
    {formId:'0001F2A3', name:'Coachman', edid:'WPN_Coachman', typeIndex:0, type:'Weapons', subIndex:3, sub:'Shotguns', plugin:'Starfield.esm', value:1099, weight:5.1},
    {formId:'002B7C10', name:'Novalight Pistol', edid:'WPN_Novalight', typeIndex:0, type:'Weapons', subIndex:1, sub:'Pistols', plugin:'ShatteredSpace.esm', value:2450, weight:1.9},
    {formId:'0010A9C4', name:'Deep Mining Spacesuit', edid:'ARMO_SpaceSuit_DeepMining', typeIndex:1, type:'Armor', subIndex:1, sub:'Spacesuits', plugin:'Starfield.esm', value:3200, weight:12.4},
    {formId:'0024E116', name:'Bounty Hunter Helmet', edid:'ARMO_Helmet_BountyHunter', typeIndex:1, type:'Armor', subIndex:2, sub:'Helmets', plugin:'Starfield.esm', value:1450, weight:3.2},
    {formId:'0012C001', name:'7.77mm Caseless', edid:'Ammo_777mm_Caseless', typeIndex:2, type:'Ammo', subIndex:1, sub:'Ballistic', plugin:'Starfield.esm', value:2, weight:0.0},
    {formId:'002FAD31', name:'Sabacc Card - The Idiot', edid:'MISC_Sabacc_Idiot', typeIndex:3, type:'Misc', subIndex:3, sub:'Cards', plugin:'Starfield.esm', value:120, weight:0.1},
    {formId:'0005BD02', name:'Vytinium Fuel Rod', edid:'MISC_Resource_Vytinium', typeIndex:3, type:'Misc', subIndex:4, sub:'Resources', plugin:'Starfield.esm', value:340, weight:2.0},
    {formId:'0011A7E3', name:'Erdebrau Pils', edid:'ALCH_Drink_ErdebrauPils', typeIndex:4, type:'Aid', subIndex:1, sub:'Food / Drink', plugin:'Starfield.esm', value:22, weight:0.5},
    {formId:'000B2C55', name:'Med Pack', edid:'ALCH_MedPack', typeIndex:4, type:'Aid', subIndex:2, sub:'Medical', plugin:'Starfield.esm', value:95, weight:0.4},
    {formId:'0020D3A9', name:'The Last Nova', edid:'BOOK_LastNova', typeIndex:5, type:'Books', subIndex:5, sub:'Misc / Other', plugin:'Starfield.esm', value:15, weight:0.3},
    {formId:'0009A3C0', name:'Keycard - Abandoned Mine', edid:'KEYM_MineAccess', typeIndex:6, type:'Keys', subIndex:1, sub:'Keycards / Access', plugin:'Starfield.esm', value:0, weight:0.0},
  ];

  let filter = {type: 8, sub: 0, plugin: -1, hideTest: true, hideHair: true, hideCrew: true, hideTurrets: true, hideBug: true, favouritesOnly: false};
  let basket = [];
  let credits = 48250;
  // Starred ids, kept across a refresh like the plugin's file-backed list.
  let favourites = [];
  // The page's sort, kept the way the plugin keeps it in its INI, so loading
  // the page again restores the column instead of resetting to catalogue order.
  // sessionStorage is the stand-in for the INI: the harness has no file, and
  // this object is rebuilt on every page load.
  let sortColumn = -1;
  let sortAscending = true;
  try {
    const saved = JSON.parse(sessionStorage.getItem('mock.osfshop.sort') || 'null');
    if (saved) { sortColumn = Number(saved.column); sortAscending = !!saved.ascending; }
  } catch (e) { /* harness */ }

  function nameOf(item) { return item.name || ('[' + item.formId + ']'); }
  function price(line) { return line.unitPrice * line.count; }
  function cost(free) { return free ? 0 : basket.reduce((sum, line) => sum + price(line), 0); }
  function basketJson() {
    return basket.map((line) => ({formId: line.formId, name: line.name, sub: line.sub, count: line.count, unitPrice: line.unitPrice, weight: line.weight || 0, omods: line.omods || 0}));
  }
  function state(free) {
    return {credits: credits, cost: cost(free), basket: basketJson(), favourites: favourites.slice()};
  }

  function rowOf(item) {
    return Object.assign({}, item, {favourite: favourites.indexOf(item.formId) >= 0});
  }

  function filtered(payload) {
    const q = String(payload.search || '').toLowerCase();
    // Starred view: every starred record, whatever category or plugin is
    // selected - the same rule the plugin's BuildFormList uses.
    if (payload.favouritesOnly) {
      return favourites
        .map((formId) => ITEMS.find((item) => item.formId === formId))
        .filter(Boolean)
        .map(rowOf);
    }
    return ITEMS.filter((item) => {
      if (payload.type !== 8 && item.typeIndex !== payload.type) return false;
      if (Number(payload.sub) !== 0 && item.subIndex !== Number(payload.sub)) return false;
      if (q && !(item.name.toLowerCase().includes(q) || item.edid.toLowerCase().includes(q) || item.formId.toLowerCase().includes(q))) return false;
      return true;
    }).map(rowOf);
  }

  // --- preview / audio / themes -----------------------------------------
  let audio = {musicVolume: 45, sfxVolume: 55, musicMuted: false, sfxMuted: false, track: '',
    tracks: ['shop_bed.wav', 'Music\\drift_loop.wav', 'Music\\neon_bazaar.wav']};
  const PALETTES = {
    'Devilz Red': ['#ff5c73', '#a52d40', '#ffd166', '#140d13', '#1d1119', '#f6e9ea', '#c79aa1', '#6d2a38'],
    'Void Blue': ['#5c9dff', '#2d5aa5', '#ffd166', '#0d1014', '#111a24', '#e9f0f6', '#9ab0c7', '#2a446d'],
    'Star Gold': ['#ffd166', '#a5822d', '#f6e9ea', '#14120d', '#1d1a11', '#fbf6ea', '#c7bd9a', '#6d5a2a'],
    'Terminal Green': ['#5cff9d', '#2da55c', '#ffd166', '#0d1410', '#111d16', '#e9f6ee', '#9ac7b0', '#2a6d44'],
    'Paper Ledger': ['#c9a227', '#7a6417', '#4a3f2a', '#f2ece0', '#e6ddc9', '#241f18', '#6b6353', '#b8ab8d'],
    'Hazard': ['#ffcf33', '#a58a1a', '#ff5c73', '#141210', '#1d1a14', '#fff8e1', '#c7b88f', '#6d5d2a'],
    'Starfield': ['#8fb3ff', '#3f5aa5', '#ffd166', '#0b0e16', '#121724', '#eaf0ff', '#9aa8c7', '#2a3a6d'],
    'Hell': ['#ff3b30', '#a51f19', '#ffb347', '#140a0a', '#1d0f0e', '#ffe9e6', '#c79a95', '#6d2a26'],
    'Heaven': ['#f2e6a0', '#a59a5c', '#fff2c4', '#141416', '#1d1e22', '#fdfbf2', '#c7c2a8', '#6d6a4a'],
  };
  const THEMES = Object.keys(PALETTES).map((name, i) => ({
    index: i, name, suffix: name.toLowerCase().replace(/ /g, '-'), premium: i >= 6,
    unlocked: i < 8, active: i === 0, progress: i === 8 ? '3 / 12 free hauls' : '',
    accent: PALETTES[name][0], accentBright: PALETTES[name][1], gold: PALETTES[name][2],
    panel: PALETTES[name][3], panelAlt: PALETTES[name][4], window: PALETTES[name][5],
    ink: PALETTES[name][5], muted: PALETTES[name][6], border: PALETTES[name][7],
  }));
  let activeTheme = 0;

  function themePayload() {
    THEMES.forEach((row) => { row.active = row.index === activeTheme; });
    return {ok: true, active: activeTheme, themes: THEMES};
  }

  function detail(formId) {
    const item = ITEMS.find((entry) => entry.formId === formId);
    if (!item) return {ok: false};
    const base = {ok: true, item: item, formType: {Weapons:'WEAP', Armor:'ARMO', Ammo:'AMMO', Misc:'MISC', Aid:'ALCH', Books:'BOOK', Keys:'KEYM'}[item.type], model: 'Weapons\\\\' + item.edid + '.nif', keywords: 'DisplayObjectTypeRifle, WeaponTypeRifle, ' + item.edid, mods: [], qualities: [], rarities: [], rarityNote: ''};
    if (item.type === 'Weapons') {
      base.mods = [
        {slot:'receiver', label:'Stock receiver', fid:'00000000', selected:false, default:true},
        {slot:'receiver', label:'Hair Trigger Receiver', fid:'0026A1C0', selected:true, default:false},
        {slot:'barrel', label:'Stock / no extra part', fid:'00000000', selected:true, default:true},
        {slot:'barrel', label:'Long Barrel', fid:'0026A1C1', selected:false, default:false},
        {slot:'barrel', label:'Short Barrel', fid:'0026A1C2', selected:false, default:false},
        {slot:'optic', label:'Stock / no extra part', fid:'00000000', selected:false, default:false},
        {slot:'optic', label:'Reflex Sight', fid:'0026A1C3', selected:true, default:true},
      ];
      base.qualities = [
        {slot:'quality', label:'Standard', fid:'00000000', selected:false, default:false},
        {slot:'quality', label:'Calibrated', fid:'0026A1D0', selected:true, default:false},
        {slot:'quality', label:'Advanced', fid:'0026A1D1', selected:false, default:false},
      ];
      base.rarities = [
        {slot:'rarity', label:'Common (no effects)', fid:'00000000', selected:false, default:false},
        {slot:'rarity', label:'Rare (1 effect)', fid:'FF000001', selected:false, default:false},
        {slot:'rarity', label:'Epic (2 effects)', fid:'FF000002', selected:true, default:false},
        {slot:'rarity', label:'Legendary (3 effects)', fid:'FF000003', selected:false, default:false},
      ];
      base.rarityNote = '9 legendary effects in this weapon\'s pool';
    } else if (item.type === 'Armor') {
      base.rarities = [
        {slot:'rarity', label:'Common (no effects)', fid:'00000000', selected:true, default:false},
        {slot:'rarity', label:'Rare (1 effect)', fid:'FF000001', selected:false, default:false},
        {slot:'rarity', label:'Epic (2 effects)', fid:'FF000002', selected:false, default:false},
        {slot:'rarity', label:'Legendary (3 effects)', fid:'FF000003', selected:false, default:false},
      ];
      base.rarityNote = '12 legendary effects in this armor pool';
    }
    return base;
  }

  // osfui.on is real here: the page settles its requests from the plugin's
  // typed push, so the harness has to deliver those the way the native side
  // does (SendToWeb -> {kind:'event'} -> osfui.on handler).
  const handlers = new Map();

  window.osfui = {
    available: function () { return true; },
    on: function (type, fn) {
      const name = String(type);
      if (!handlers.has(name)) handlers.set(name, new Set());
      handlers.get(name).add(fn);
      return function () { handlers.get(name).delete(fn); };
    },
    send: function () { return true; },
    emit: function () { return true; },
    request: function (name, payload) {
      const p = payload || {};
      const free = p.free === undefined ? true : !!p.free;
      if (name === 'devilzdad.osfshop.catalog' || name === 'devilzdad.osfshop.refresh') {
        if (name.endsWith('refresh')) { basket = []; filter = {type: 8, sub: 0, plugin: -1, hideTest: true, hideHair: true, hideCrew: true, hideTurrets: true, hideBug: true, favouritesOnly: false}; }
        // The plugin keeps the page's sort in its INI and reports it here; the
        // harness keeps it in memory so a reload of the page exercises the
        // same restore path without touching a file.
        return Promise.resolve(Object.assign({ok: true, version: '0.8.350 (mock)', categories: CATALOG, plugins: PLUGINS, filter: filter, sortColumn: sortColumn, sortAscending: sortAscending, total: ITEMS.length}, state(free)));
      }
      if (name === 'devilzdad.osfshop.rows') {
        filter = {type: Number(p.type), sub: Number(p.sub), plugin: Number(p.plugin), hideTest: !!p.hideTest, hideHair: !!p.hideHair, hideCrew: !!p.hideCrew, hideTurrets: !!p.hideTurrets, hideBug: !!p.hideBug, favouritesOnly: !!p.favouritesOnly};
        const all = filtered(p);
        // The header the page clicked, with the plugin's meaning: 0 name,
        // 1 shelf, 2 type, 3 value, 4 weight, 5 FormID, -1 catalogue order.
        // The native side sorts the whole filtered catalogue before it cuts
        // the window, so the harness has to do that too or a page turn would
        // look sorted while page 2 would not be.
        const sort = Number(p.sort === undefined ? -1 : p.sort);
        const asc = p.asc === undefined ? true : !!p.asc;
        sortColumn = sort;
        sortAscending = asc;
        try {
          sessionStorage.setItem('mock.osfshop.sort', JSON.stringify({column: sortColumn, ascending: sortAscending}));
        } catch (e) { /* harness */ }
        if (sort >= 0) {
          const keyOf = (row) => {
            if (sort === 0) return String(row.name || '').trim().toLowerCase();
            if (sort === 1) return String(row.sub || '').toLowerCase();
            if (sort === 2) return String(row.type || '').toLowerCase();
            if (sort === 3) return Number(row.value || 0);
            if (sort === 4) return Number(row.weight || 0);
            return parseInt(String(row.formId), 16) || 0;
          };
          const keys = new Map(all.map((row) => [row, keyOf(row)]));
          all.sort((a, b) => {
            const ka = keys.get(a), kb = keys.get(b);
            let cmp = ka < kb ? -1 : ka > kb ? 1 : 0;
            if (cmp === 0) {
              const ia = parseInt(String(a.formId), 16) || 0;
              const ib = parseInt(String(b.formId), 16) || 0;
              cmp = ia < ib ? -1 : ia > ib ? 1 : 0;
            }
            return asc ? cmp : -cmp;
          });
        }
        const offset = Number(p.offset || 0);
        const limit = Number(p.limit || 100);
        return Promise.resolve(Object.assign({ok: true, total: all.length, offset: offset, rows: all.slice(offset, offset + limit)}, state(free)));
      }
      if (name === 'devilzdad.osfshop.favourite') {
        const item = ITEMS.find((entry) => entry.formId === p.formId);
        if (!item) return Promise.resolve({ok: false, error: 'that record is not in the load order'});
        const at = favourites.indexOf(item.formId);
        if (at >= 0) favourites.splice(at, 1);
        else favourites.push(item.formId);
        return Promise.resolve(Object.assign({ok: true, favourite: at < 0}, state(free)));
      }
      if (name === 'devilzdad.osfshop.detail') {
        return Promise.resolve(detail(p.formId));
      }
      if (name === 'devilzdad.osfshop.configure') {
        const next = detail(p.formId);
        const group = p.kind === 'mod' ? 'mods' : (p.kind === 'quality' ? 'qualities' : 'rarities');
        (next[group] || []).forEach((option) => {
          const sameSlot = option.slot === p.slot;
          if (sameSlot) option.selected = option.fid === p.fid;
        });
        return Promise.resolve({ok: true, detail: next});
      }
      if (name === 'devilzdad.osfshop.basket') {
        const item = ITEMS.find((entry) => entry.formId === p.formId);
        if (p.op === 'add' && item) {
          if (basket.length >= 25 && !basket.some((line) => line.formId === item.formId)) {
            return Promise.resolve(Object.assign({ok: false, error: 'basket is full (25 lines)'}, state(free)));
          }
          const line = basket.find((entry) => entry.formId === item.formId);
          if (line) line.count = Math.min(999, line.count + Number(p.count || 1));
          else basket.push({formId: item.formId, name: item.name, sub: item.sub, count: Number(p.count || 1), unitPrice: item.value, weight: item.weight || 0});
        } else if (p.op === 'set') {
          const line = basket.find((entry) => entry.formId === p.formId);
          if (line) line.count = Math.max(1, Math.min(999, Number(p.count || 1)));
        } else if (p.op === 'remove') {
          basket.splice(Number(p.index || 0), 1);
        } else if (p.op === 'clear') {
          basket = [];
        }
        return Promise.resolve(Object.assign({ok: true, error: ''}, state(free)));
      }
      if (name === 'devilzdad.osfshop.accept') {
        if (!basket.length) return Promise.resolve(Object.assign({ok: false, lines: ['Basket is empty.']}, state(free)));
        const total = cost(free);
        if (!free && total > credits) return Promise.resolve(Object.assign({ok: false, lines: ['transfer refused: need ' + total + ' credits, have ' + credits]}, state(free)));
        if (!free) credits -= total;
        const lines = basket.map((line) => (free ? 'added' : 'bought') + ' x' + line.count + ' ' + line.name);
        basket = [];
        return Promise.resolve(Object.assign({ok: true, lines: lines}, state(free)));
      }
      if (name === 'devilzdad.osfshop.preview') {
        const item = ITEMS.find((entry) => entry.formId === p.formId);
        if (!item) return Promise.resolve({ok: false, error: 'that record is not in the load order'});
        const yaw = Math.round(Number(p.yaw || 0));
        const pitch = Math.round(Number(p.pitch || 0));
        const zoom = Math.round(Number(p.zoom || 1) * 100);
        window.__previewCalls = (window.__previewCalls || 0) + 1;
        window.__lastCamera = [yaw, pitch, zoom];
        // The native side answers with the PNG itself as a data URL - a file
        // written beside the page is never served, because OSF UI snapshots
        // the view at startup. This stand-in draws the same shaded ball the
        // native renderer's geometry would put there, with the highlight
        // walking with the yaw so a rotate is visibly a new frame.
        return Promise.resolve({
          ok: true,
          data: standInPreview(yaw, zoom / 100),
          width: 320, height: 320, error: ''});
      }
      if (name === 'devilzdad.osfshop.audio') {
        if (p.op === 'set') {
          audio = {musicVolume: Number(p.musicVolume), sfxVolume: Number(p.sfxVolume),
            musicMuted: !!p.musicMuted, sfxMuted: !!p.sfxMuted, track: String(p.track || ''), tracks: audio.tracks};
        }
        return Promise.resolve(Object.assign({ok: true}, audio));
      }
      if (name === 'devilzdad.osfshop.themes') {
        return Promise.resolve(themePayload());
      }
      if (name === 'devilzdad.osfshop.themepick') {
        const row = THEMES.find((entry) => entry.index === Number(p.index));
        if (!row) return Promise.resolve({ok: false, error: 'no such theme'});
        if (row.premium && !row.unlocked) {
          return Promise.resolve(Object.assign(themePayload(), {ok: false, error: 'that theme is still locked'}));
        }
        activeTheme = row.index;
        return Promise.resolve(themePayload());
      }
      return Promise.reject(new Error('unknown endpoint ' + name));
    },
  };
  // A shaded ball whose highlight follows the yaw, drawn on a canvas and
  // returned as a PNG data URL exactly the way the plugin answers.
  function standInPreview(yawDegrees, zoom) {
    const canvas = document.createElement('canvas');
    const size = 320;
    canvas.width = canvas.height = size;
    const ctx = canvas.getContext('2d');
    const centre = size / 2;
    const radius = size * 0.34 * Math.max(0.4, Math.min(2.5, zoom || 1));
    const yaw = (yawDegrees || 0) * Math.PI / 180;
    const light = [centre + Math.cos(yaw) * radius * 0.45, centre + Math.sin(yaw) * radius * 0.25];
    for (let y = 0; y < size; y += 1) {
      for (let x = 0; x < size; x += 1) {
        const dx = x - centre;
        const dy = y - centre;
        if (dx * dx + dy * dy > radius * radius) {
          const v = 18 + Math.round(10 * (y / size));
          ctx.fillStyle = 'rgb(' + v + ',' + v + ',' + (v + 6) + ')';
        } else {
          const lx = x - light[0];
          const ly = y - light[1];
          const shade = Math.max(0, 1 - Math.sqrt(lx * lx + ly * ly) / (radius * 1.5));
          const base = 90 + Math.round(120 * shade);
          ctx.fillStyle = 'rgb(' + Math.min(255, base + 30) + ',' + Math.min(255, base - 10) + ',' + Math.min(255, base + 55) + ')';
        }
        ctx.fillRect(x, y, 1, 1);
      }
    }
    return canvas.toDataURL('image/png');
  }

  window.osfui.call = function (name, payload) { return window.osfui.request(name, payload); };

  // The raw transport the page actually uses: it posts envelopes itself and
  // settles them from onMessage, so it does not care which helper kit (2.0 or
  // the 1.6 one that ships beside legacy views) happens to be loaded.
  window.osfui.state = {
    on: function (key, fn) {
      if (String(key).toLowerCase() === 'osfui/views') {
        try { fn({views: [{id: 'devilzdad.osfshop/shop', open: true}]}); } catch (e) { /* harness */ }
      }
      return function () {};
    },
  };
  window.osfui.postMessage = function (json) {
    let env;
    try { env = JSON.parse(json); } catch (e) { return; }
    if (!env || !env.name) return;
    // Mirrors the helper's own switch: an event envelope is dispatched to the
    // osfui.on subscribers, which is how the real kit delivers a SendToWeb push.
    const deliver = function (message) {
      if (message && message.kind === 'event') {
        const set = handlers.get(String(message.name));
        if (set) set.forEach((fn) => { try { fn(message.payload, message); } catch (e) { console.error(e); } });
        return;
      }
      if (typeof window.osfui.onMessage === 'function') window.osfui.onMessage(JSON.stringify(message));
    };
    Promise.resolve()
      .then(function () { return window.osfui.request(env.name, env.payload || {}); })
      .then(function (payload) {
        if (env.kind !== 'request') return;
        // Exactly what the plugin's SendToWeb push carries.
        deliver({kind: 'event', name: 'devilzdad.osfshop.reply', payload: {id: env.id, payload: payload}});
      }, function (err) {
        if (env.kind !== 'request') return;
        deliver({kind: 'event', name: 'devilzdad.osfshop.reply', payload: {id: env.id, payload: {ok: false, error: String((err && err.message) || err)}}});
      });
  };
})();
"""


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=ROOT, **kwargs)

    def end_headers(self):
        # The harness is edited between runs; heuristic browser caching would
        # otherwise serve the previous CSS/JS and hide a change.
        if self.path.split("?")[0].endswith((".css", ".js", ".html", "/")):
            self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def do_GET(self):
        path = self.path.split("?")[0]
        if path == "/shared/osfui.js":
            body = MOCK_JS.encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/javascript; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)
            return
        super().do_GET()

    def log_message(self, fmt, *args):
        sys.stderr.write("[mock] " + (fmt % args) + "\n")


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8791
    if not os.path.isdir(ROOT):
        raise SystemExit("view folder not found: " + ROOT)
    with http.server.ThreadingHTTPServer(("127.0.0.1", port), Handler) as server:
        print("mock shop on http://127.0.0.1:%d/ (root %s)" % (port, ROOT), flush=True)
        server.serve_forever()


if __name__ == "__main__":
    main()
