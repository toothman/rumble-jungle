'use strict';
/*
 * Rumble Jungle web UI. Plain JS, no framework.
 * Settings are flat key/value pairs (same keys as MQTT). Controls carry a
 * data-key attribute and save themselves; forms save on submit.
 */

const $ = (sel, el = document) => el.querySelector(sel);
const $$ = (sel, el = document) => [...el.querySelectorAll(sel)];

const state = {
  settings: null,   // flat settings object
  fields: {},       // metadata per key: min/max/step/unit/options
  status: null,
  rev: -1,
  fw: null,
  bands: 5,
  fs: 44100,
  busy: 0,          // >0 while a slider is being dragged
  band: null,       // selected EQ band (0-based) or null
  dirtyForms: new Set(),
  offline: false,
};

/* ------------------------------------------------------------------ API */

async function api(method, url, body) {
  const ctrl = new AbortController();
  const timer = setTimeout(() => ctrl.abort(), 8000);
  try {
    const res = await fetch(url, {
      method,
      signal: ctrl.signal,
      headers: body ? { 'Content-Type': 'application/json' } : undefined,
      body: body ? JSON.stringify(body) : undefined,
    });
    const data = await res.json().catch(() => ({}));
    if (!res.ok) throw new Error(data.error || `HTTP ${res.status}`);
    return data;
  } finally {
    clearTimeout(timer);
  }
}

async function save(patch, quiet = false) {
  try {
    const data = await api('PATCH', '/api/settings', patch);
    applySettings(data);
    if (!quiet) toast('Saved');
    return true;
  } catch (e) {
    toast(e.message, 'error');
    render();   // revert controls to the stored values
    return false;
  }
}

function applySettings(data) {
  state.settings = data.settings;
  state.fields = data.fields;
  state.bands = data.eq_bands;
  state.fs = data.sample_rate;
  state.rev = data.settings_rev;
  render();
}

/* ------------------------------------------------------------ Formatting */

const LABELS = {
  rumble_freq: 'Cutoff', output_gain: 'Gain', xfade_ms: 'Crossfade',
  signal_on_ms: 'Signal on after', signal_off_ms: 'Signal off after silence of',
};

function fmt(key, v) {
  const unit = state.fields[key]?.unit;
  if (unit === 'dB') return `${v > 0 ? '+' : ''}${(+v).toFixed(1)} dB`;
  if (unit === 'dBFS') return `${Math.round(v)} dBFS`;
  if (unit === 'Hz') return fmtHz(v);
  if (unit === 'ms') return v >= 1000 ? `${+(v / 1000).toFixed(1)} s` : `${v} ms`;
  if (key.endsWith('_q')) return `Q ${(+v).toFixed(2)}`;
  return String(v);
}

function fmtHz(v) {
  if (v >= 10000) return `${Math.round(v / 1000)} kHz`;
  if (v >= 1000) return `${+(v / 1000).toFixed(1)} kHz`;
  return `${Math.round(v)} Hz`;
}

function esc(s) {
  return String(s).replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
}

const inputName = i => state.settings?.[`in_${'ab'[i]}_name`] || `Input ${'AB'[i]}`;

/* ----------------------------------------------------------------- Toast */

let toastTimer;
function toast(msg, kind = 'ok') {
  const box = $('#toast');
  const color = kind === 'error'
    ? 'border-ember/50 bg-ember text-night'
    : 'border-lime/40 bg-moss text-dew';
  box.innerHTML = `<div class="animate-pop flex items-center gap-2 rounded-2xl border px-4 py-2.5 text-sm font-semibold shadow-2xl ${color}">
    ${kind === 'error' ? '!' : '<svg class="size-4 text-lime" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="3"><path d="m5 13 4 4L19 7"/></svg>'}
    <span>${esc(msg)}</span></div>`;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { box.innerHTML = ''; }, kind === 'error' ? 4000 : 1400);
}

/* ---------------------------------------------------------------- Dialog */

function ask({ title, text, ok = 'OK', danger = false, confirmWord = null }) {
  const dlg = $('#dlg'), input = $('#dlg-input'), btn = $('#dlg-ok');
  $('#dlg-title').textContent = title;
  $('#dlg-text').textContent = text;
  btn.textContent = ok;
  btn.className = danger ? 'btn-danger' : 'btn-primary';
  input.hidden = !confirmWord;
  input.value = '';
  input.placeholder = confirmWord ? `Type ${confirmWord}` : '';
  btn.disabled = !!confirmWord;
  input.oninput = () => { btn.disabled = input.value.trim() !== confirmWord; };
  dlg.showModal();
  return new Promise(resolve => {
    dlg.onclose = () => resolve(dlg.returnValue === 'ok');
  });
}

/* ------------------------------------------------------------------ Tabs */

const ICONS = {
  live: '<path d="M12 3a9 9 0 1 0 0 18 9 9 0 0 0 0-18Z"/><circle cx="12" cy="12" r="3"/>',
  sound: '<path d="M4 21v-7M4 10V3M12 21v-9M12 8V3M20 21v-5M20 12V3M1 14h6M9 8h6M17 16h6"/>',
  inputs: '<path d="M9 18V5l12-2v13"/><circle cx="6" cy="18" r="3"/><circle cx="18" cy="16" r="3"/>',
  connect: '<path d="M5 12.55a11 11 0 0 1 14 0M1.42 9a16 16 0 0 1 21.16 0M8.53 16.11a6 6 0 0 1 6.95 0"/><circle cx="12" cy="20" r="1"/>',
  system: '<path d="M12 15a3 3 0 1 0 0-6 3 3 0 0 0 0 6Z"/><path d="M19.4 15a1.7 1.7 0 0 0 .3 1.8l.1.1a2 2 0 1 1-2.8 2.8l-.1-.1a1.7 1.7 0 0 0-1.8-.3 1.7 1.7 0 0 0-1 1.5V21a2 2 0 1 1-4 0v-.1a1.7 1.7 0 0 0-1.1-1.5 1.7 1.7 0 0 0-1.8.3l-.1.1a2 2 0 1 1-2.8-2.8l.1-.1a1.7 1.7 0 0 0 .3-1.8 1.7 1.7 0 0 0-1.5-1H3a2 2 0 1 1 0-4h.1a1.7 1.7 0 0 0 1.5-1.1 1.7 1.7 0 0 0-.3-1.8l-.1-.1a2 2 0 1 1 2.8-2.8l.1.1a1.7 1.7 0 0 0 1.8.3H9a1.7 1.7 0 0 0 1-1.5V3a2 2 0 1 1 4 0v.1a1.7 1.7 0 0 0 1 1.5 1.7 1.7 0 0 0 1.8-.3l.1-.1a2 2 0 1 1 2.8 2.8l-.1.1a1.7 1.7 0 0 0-.3 1.8V9a1.7 1.7 0 0 0 1.5 1H21a2 2 0 1 1 0 4h-.1a1.7 1.7 0 0 0-1.5 1Z"/>',
};
const TABS = [['live', 'Live'], ['sound', 'Sound'], ['inputs', 'Inputs'], ['connect', 'Connect'], ['system', 'System']];

function buildTabs() {
  const html = TABS.map(([id, label]) =>
    `<button class="tab" role="tab" data-tab="${id}" aria-selected="false">
       <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">${ICONS[id]}</svg>
       <span>${label}</span></button>`).join('');
  $('#tabs-top').innerHTML = html;
  $('#tabs-bottom').innerHTML = html;
  document.addEventListener('click', e => {
    const t = e.target.closest('[data-tab]');
    if (t) location.hash = t.dataset.tab;
  });
  window.addEventListener('hashchange', showTab);
  showTab();
}

function currentTab() {
  const id = location.hash.slice(1);
  return TABS.some(([t]) => t === id) ? id : 'live';
}

function showTab() {
  const id = currentTab();
  $$('[data-page]').forEach(p => { p.hidden = p.dataset.page !== id; });
  $$('[data-tab]').forEach(t => t.setAttribute('aria-selected', t.dataset.tab === id));
  window.scrollTo({ top: 0 });
  schedulePoll(0);
}

/* --------------------------------------------------------------- Sliders */

// Log-scale frequency sliders use 0..1000 internally
const LOG_STEPS = 1000;
const toLog = (v, min, max) => Math.round(Math.log(v / min) / Math.log(max / min) * LOG_STEPS);
const fromLog = (p, min, max) => min * Math.pow(max / min, p / LOG_STEPS);

function sliderHTML(key, { label, log = false, vertical = false } = {}) {
  const f = state.fields[key] || {};
  const min = log ? 0 : f.min, max = log ? LOG_STEPS : f.max, step = log ? 1 : f.step;
  if (vertical) {
    return `<input type="range" class="v" data-range="${key}" min="${min}" max="${max}" step="${step}" aria-label="${esc(f.name || key)}">`;
  }
  return `<div class="flex items-baseline justify-between gap-3">
      <label class="label">${esc(label || LABELS[key] || f.name || key)}</label>
      <span class="font-mono text-sm font-semibold text-lime" data-show="${key}"></span>
    </div>
    <input type="range" data-range="${key}" ${log ? 'data-log' : ''} min="${min}" max="${max}" step="${step}">`;
}

function setRangeFill(el) {
  const min = +el.min, max = +el.max, v = +el.value;
  const pct = (v - min) / (max - min) * 100;
  if (el.classList.contains('v')) {
    const zero = (0 - min) / (max - min) * 100;
    el.style.setProperty('--lo', `${Math.min(zero, pct)}%`);
    el.style.setProperty('--hi', `${Math.max(zero, pct)}%`);
  } else {
    el.style.setProperty('--pct', `${pct}%`);
  }
}

function rangeValue(el) {
  const key = el.dataset.range, f = state.fields[key];
  if (el.hasAttribute('data-log')) {
    const v = fromLog(+el.value, f.min, f.max);
    return v >= 1000 ? Math.round(v / 10) * 10 : Math.round(v);
  }
  return +el.value;
}

// Live updates while dragging (throttled), final save on release
const pending = {};
function onRangeInput(el) {
  const key = el.dataset.range;
  const v = rangeValue(el);
  setRangeFill(el);
  state.settings[key] = v;
  $$(`[data-show="${key}"]`).forEach(s => { s.textContent = fmt(key, v); });
  if (key.startsWith('eq') || key.startsWith('rumble')) drawEq();
  if (pending[key]) return;
  pending[key] = setTimeout(() => {
    delete pending[key];
    save({ [key]: state.settings[key] }, true);
  }, 150);
}

/* -------------------------------------------------------------- Controls */

function segHTML(key, labels) {
  const opts = state.fields[key]?.options || [];
  return opts.map((o, i) =>
    `<button type="button" data-seg="${key}" data-value="${o.value}" aria-pressed="false">${esc(labels ? labels[i] : o.label)}</button>`
  ).join('');
}

function buildControls() {
  const names = [inputName(0), inputName(1)];
  $('#seg-source').innerHTML = segHTML('source_mode', ['Auto', names[0], names[1], 'Mix']);
  $('#seg-priority').innerHTML = segHTML('priority', names);

  $$('[data-slider]').forEach(el => { el.innerHTML = sliderHTML(el.dataset.slider); });

  const sel = $('select[data-key="bitrate"]');
  sel.innerHTML = (state.fields.bitrate?.options || [])
    .map(o => `<option value="${o.value}">${esc(o.label)}</option>`).join('');

  buildEq();
  buildInputs();
}

function buildInputs() {
  const colors = ['bg-lime', 'bg-sky'];
  let live = '<div class="card-title">Inputs</div><div class="mt-4 grid gap-4">';
  let cards = '';
  for (let i = 0; i < 2; i++) {
    const id = 'ab'[i];
    live += `<div>
        <div class="mb-1.5 flex items-center gap-2 text-sm">
          <span class="size-2 rounded-full ${colors[i]}"></span>
          <span class="flex-1 truncate font-semibold" data-name="${i}"></span>
          <span class="font-mono text-xs text-mist" data-level="${i}">—</span>
          <span class="rounded-full px-2 py-0.5 text-[10px] font-bold uppercase" data-sig="${i}"></span>
        </div>
        <div class="meter"><div class="meter-fill" data-meter="${i}"></div><div class="meter-mark" data-thr="${i}"></div></div>
      </div>`;
    cards += `<div class="card">
        <div class="flex items-center gap-3">
          <span class="grid size-9 place-items-center rounded-xl ${colors[i]} text-sm font-black text-night">${'AB'[i]}</span>
          <input class="input !bg-transparent !border-transparent !px-2 text-lg font-bold hover:!border-fern focus:!border-lime/70"
                 data-key="in_${id}_name" maxlength="23" aria-label="Input name">
          <span class="rounded-full px-2 py-0.5 text-[10px] font-bold uppercase" data-sig="${i}"></span>
        </div>
        <div class="mt-4">
          <div class="mb-1.5 flex justify-between text-xs text-mist"><span>Level</span><span class="font-mono" data-level="${i}">—</span></div>
          <div class="meter !h-4"><div class="meter-fill" data-meter="${i}"></div><div class="meter-mark !bg-sun" data-thr="${i}"></div></div>
          <div class="mt-1 flex justify-between font-mono text-[10px] text-mist/60"><span>−90</span><span>−60</span><span>−30</span><span>0 dB</span></div>
        </div>
        <div class="mt-4">${sliderHTML(`in_${id}_threshold`, { label: 'Signal threshold (yellow mark)' })}</div>
        <p class="hint">Set it between the level when the source is stopped and the level while music plays.</p>
        <div class="mt-4">${sliderHTML(`in_${id}_trim`, { label: 'Input trim' })}</div>
      </div>`;
  }
  $('#live-meters').innerHTML = live + '</div>';
  $('#input-cards').innerHTML = cards;
}

/* -------------------------------------------------------------------- EQ */

const BAND_COLORS = ['#b8f36b', '#6ad4ff', '#c79bff', '#ffc35c', '#ff8fa3', '#7cf0c8', '#ffd86b', '#9fb6ff', '#ff9f6b', '#d0ff8b'];

function buildEq() {
  const wrap = $('#eq-bands');
  wrap.style.setProperty('--bands', state.bands);
  let html = '';
  for (let b = 1; b <= state.bands; b++) {
    html += `<div class="flex flex-col items-center gap-1">
        <span class="font-mono text-[11px] font-semibold text-dew" data-show="eq${b}_gain"></span>
        ${sliderHTML(`eq${b}_gain`, { vertical: true })}
        <button type="button" class="rounded-lg px-1.5 py-1 font-mono text-[11px] font-semibold text-mist hover:bg-fern/60 aria-pressed:bg-fern aria-pressed:text-dew"
                data-band="${b - 1}" aria-pressed="false">
          <span class="mr-0.5 inline-block size-1.5 rounded-full align-middle" style="background:${BAND_COLORS[b - 1]}"></span><span data-show="eq${b}_freq"></span>
        </button>
      </div>`;
  }
  wrap.innerHTML = html;
}

function showBandDetail() {
  const box = $('#eq-detail');
  $$('[data-band]').forEach(b => b.setAttribute('aria-pressed', +b.dataset.band === state.band));
  if (state.band === null) { box.hidden = true; return; }
  const n = state.band + 1;
  const type = n === 1 ? 'Low shelf' : n === state.bands ? 'High shelf' : 'Peak';
  box.innerHTML = `<div class="flex items-center gap-2">
      <span class="size-2.5 rounded-full" style="background:${BAND_COLORS[state.band]}"></span>
      <p class="flex-1 text-sm font-semibold">Band ${n} · <span class="text-mist">${type}</span></p>
      <button type="button" class="btn-ghost !px-2.5 !py-1 !text-xs" data-band-close>Close</button>
    </div>
    <div class="mt-3 grid gap-4 sm:grid-cols-3">
      <div>${sliderHTML(`eq${n}_gain`, { label: 'Gain' })}</div>
      <div>${sliderHTML(`eq${n}_freq`, { label: 'Frequency', log: true })}</div>
      <div>${sliderHTML(`eq${n}_q`, { label: 'Width (Q)' })}</div>
    </div>`;
  box.hidden = false;
  render();
}

// Biquad design, same formulas as the firmware (RBJ Audio EQ Cookbook)
function biquad(type, f0, q, gainDb, fs) {
  const A = Math.pow(10, gainDb / 40), w0 = 2 * Math.PI * f0 / fs;
  const cw = Math.cos(w0), alpha = Math.sin(w0) / (2 * q), sa = 2 * Math.sqrt(A) * alpha;
  let b0, b1, b2, a0, a1, a2;
  if (type === 'ls') {
    b0 = A * ((A + 1) - (A - 1) * cw + sa); b1 = 2 * A * ((A - 1) - (A + 1) * cw); b2 = A * ((A + 1) - (A - 1) * cw - sa);
    a0 = (A + 1) + (A - 1) * cw + sa; a1 = -2 * ((A - 1) + (A + 1) * cw); a2 = (A + 1) + (A - 1) * cw - sa;
  } else if (type === 'hs') {
    b0 = A * ((A + 1) + (A - 1) * cw + sa); b1 = -2 * A * ((A - 1) + (A + 1) * cw); b2 = A * ((A + 1) + (A - 1) * cw - sa);
    a0 = (A + 1) - (A - 1) * cw + sa; a1 = 2 * ((A - 1) - (A + 1) * cw); a2 = (A + 1) - (A - 1) * cw - sa;
  } else if (type === 'hp') {
    b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = (1 + cw) / 2; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
  } else {
    b0 = 1 + alpha * A; b1 = -2 * cw; b2 = 1 - alpha * A; a0 = 1 + alpha / A; a1 = -2 * cw; a2 = 1 - alpha / A;
  }
  return [b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0];
}

function magDb(c, f, fs) {
  const w = 2 * Math.PI * f / fs, c1 = Math.cos(w), s1 = Math.sin(w), c2 = Math.cos(2 * w), s2 = Math.sin(2 * w);
  const nr = c[0] + c[1] * c1 + c[2] * c2, ni = -(c[1] * s1 + c[2] * s2);
  const dr = 1 + c[3] * c1 + c[4] * c2, di = -(c[3] * s1 + c[4] * s2);
  return 10 * Math.log10((nr * nr + ni * ni) / (dr * dr + di * di));
}

function eqFilters() {
  const s = state.settings, fs = state.fs, list = [];
  if (s.rumble) {
    list.push(biquad('hp', s.rumble_freq, 0.5412, 0, fs), biquad('hp', s.rumble_freq, 1.3066, 0, fs));
  }
  if (s.eq) {
    for (let b = 1; b <= state.bands; b++) {
      const g = s[`eq${b}_gain`];
      if (Math.abs(g) < 0.05) continue;
      const type = b === 1 ? 'ls' : b === state.bands ? 'hs' : 'pk';
      list.push(biquad(type, s[`eq${b}_freq`], s[`eq${b}_q`], g, fs));
    }
  }
  return list;
}

function drawEq() {
  const svg = $('#eq-curve');
  if (!svg || !state.settings) return;
  const W = 600, H = 160, fMin = 20, fMax = 20000, dbRange = 15;
  const x = f => Math.log(f / fMin) / Math.log(fMax / fMin) * W;
  const y = db => H / 2 - Math.max(-dbRange, Math.min(dbRange, db)) / dbRange * (H / 2 - 6);
  const filters = eqFilters();
  const resp = f => filters.reduce((sum, c) => sum + magDb(c, f, state.fs), 0);

  let grid = '';
  for (const db of [-12, -6, 6, 12]) {
    grid += `<line x1="0" x2="${W}" y1="${y(db)}" y2="${y(db)}" stroke="#1c4430" stroke-dasharray="2 6"/>`;
  }
  grid += `<line x1="0" x2="${W}" y1="${y(0)}" y2="${y(0)}" stroke="#2d6a4a"/>`;
  for (const [f, l] of [[50, '50'], [100, '100'], [500, '500'], [1000, '1k'], [5000, '5k'], [10000, '10k']]) {
    grid += `<line x1="${x(f)}" x2="${x(f)}" y1="0" y2="${H}" stroke="#1c4430" stroke-dasharray="2 6"/>
             <text x="${x(f) + 4}" y="${H - 4}" fill="#8fb3a0" font-size="10" font-family="ui-monospace,monospace">${l}</text>`;
  }

  const pts = [];
  for (let i = 0; i <= 160; i++) {
    const f = fMin * Math.pow(fMax / fMin, i / 160);
    pts.push(`${x(f).toFixed(1)},${y(resp(f)).toFixed(1)}`);
  }
  const line = `M${pts.join('L')}`;
  const area = `${line}L${W},${y(0)}L0,${y(0)}Z`;

  let dots = '';
  if (state.settings.eq) {
    for (let b = 1; b <= state.bands; b++) {
      const f = state.settings[`eq${b}_freq`];
      const sel = state.band === b - 1;
      dots += `<circle cx="${x(f)}" cy="${y(resp(f))}" r="${sel ? 7 : 5}" fill="${BAND_COLORS[b - 1]}"
                       stroke="#06110c" stroke-width="3" class="cursor-pointer" data-band="${b - 1}"/>`;
    }
  }

  svg.innerHTML = `<defs><linearGradient id="eqg" x1="0" x2="0" y1="0" y2="1">
      <stop offset="0" stop-color="#b8f36b" stop-opacity=".35"/><stop offset=".5" stop-color="#b8f36b" stop-opacity=".02"/>
      <stop offset="1" stop-color="#b8f36b" stop-opacity=".35"/></linearGradient></defs>
    ${grid}<path d="${area}" fill="url(#eqg)"/>
    <path d="${line}" fill="none" stroke="#b8f36b" stroke-width="2.5" stroke-linejoin="round" vector-effect="non-scaling-stroke"/>
    ${dots}`;
  svg.classList.toggle('opacity-40', !state.settings.eq && !state.settings.rumble);
}

/* ---------------------------------------------------------------- Render */

function isEditing(el) {
  return el === document.activeElement && (el.tagName === 'INPUT' || el.tagName === 'SELECT') && el.type !== 'checkbox';
}

function render() {
  const s = state.settings;
  if (!s) return;

  $$('[data-key]').forEach(el => {
    const key = el.dataset.key, v = s[key];
    if (v === undefined || isEditing(el)) return;
    if (el.type === 'checkbox') el.checked = !!v;
    else if (el.tagName === 'INPUT' || el.tagName === 'SELECT') el.value = v;
  });
  $$('[data-seg]').forEach(b => b.setAttribute('aria-pressed', s[b.dataset.seg] === b.dataset.value));
  $$('[data-range]').forEach(el => {
    const key = el.dataset.range, f = state.fields[key];
    if (s[key] === undefined || state.busy) return;
    el.value = el.hasAttribute('data-log') ? toLog(s[key], f.min, f.max) : s[key];
    setRangeFill(el);
  });
  $$('[data-show]').forEach(el => {
    const key = el.dataset.show;
    if (s[key] !== undefined) el.textContent = fmt(key, s[key]);
  });
  $$('[data-name]').forEach(el => { el.textContent = inputName(+el.dataset.name); });
  $$('[data-thr]').forEach(el => {
    el.style.left = `${dbToPct(s[`in_${'ab'[+el.dataset.thr]}_threshold`])}%`;
  });

  // Rebuild segment labels when input names change
  const names = [inputName(0), inputName(1)].join('|');
  if (names !== state.lastNames) {
    state.lastNames = names;
    $('#seg-source').innerHTML = segHTML('source_mode', ['Auto', inputName(0), inputName(1), 'Mix']);
    $('#seg-priority').innerHTML = segHTML('priority', [inputName(0), inputName(1)]);
    $$('[data-seg]').forEach(b => b.setAttribute('aria-pressed', s[b.dataset.seg] === b.dataset.value));
  }

  renderForms();
  drawEq();
  renderStatus();
}

function renderForms() {
  const s = state.settings;
  $$('form[data-form]').forEach(form => {
    if (state.dirtyForms.has(form.dataset.form)) return;
    $$('[name]', form).forEach(el => {
      const v = s[el.name];
      if (v === undefined || el === document.activeElement) return;
      if (el.type === 'checkbox') el.checked = !!v;
      else el.value = v;
    });
  });
  const pw = $('[name="mqtt_password"]');
  pw.placeholder = s.mqtt_password_set ? '•••••• (unchanged)' : '';
  updateStaticFields();
}

function updateStaticFields() {
  const dhcp = $('[name="dhcp"]').checked;
  $$('#static-fields input').forEach(i => { i.disabled = dhcp; });
}

// Meters span -90..0 dBFS, the same range as the signal threshold
const dbToPct = db => Math.max(0, Math.min(100, (db + 90) / 90 * 100));

const SOURCE_STYLE = {
  a: { color: 'var(--color-lime)', letter: 'A' },
  b: { color: 'var(--color-sky)', letter: 'B' },
  mix: { color: 'var(--color-orchid)', letter: 'A+B' },
  none: { color: 'var(--color-fern)', letter: '–' },
};

function renderStatus() {
  const st = state.status, s = state.settings;
  if (!st || !s) return;

  // On-air hero
  const act = st.active_source;
  const style = SOURCE_STYLE[act] || SOURCE_STYLE.none;
  const playing = st.inputs.some(i => i.signal) || act === 'a' || act === 'b' || act === 'mix';
  const spinning = act !== 'none' && st.inputs.some(i => i.signal);
  for (const disc of [$('#disc'), $('#logo-disc')]) {
    disc.style.setProperty('--disc-label', style.color);
    disc.classList.toggle('animate-spin-slow', spinning);
  }
  $('#disc-letter').textContent = style.letter;
  const name = act === 'a' ? inputName(0) : act === 'b' ? inputName(1) : act === 'mix' ? 'Mix' : 'Silence';
  $('#now-name').textContent = name;
  const prio = inputName(s.priority === 'b' ? 1 : 0);
  $('#now-sub').textContent = {
    auto: `Automatic · priority ${prio}`,
    a: `Fixed to ${inputName(0)}`,
    b: `Fixed to ${inputName(1)}`,
    mix: 'Mixing both inputs',
  }[s.source_mode] || '';
  $('#hdr-sub').textContent = spinning ? `${name} on air · ${st.listeners} listening` : (playing ? name : 'Idle');

  $('#chip-listeners').textContent = `${st.listeners} listener${st.listeners === 1 ? '' : 's'}`;
  $('#chip-clip').hidden = !st.clipping;
  $('#clip-led').className = `size-4 rounded-full transition ${st.clipping ? 'bg-ember shadow-[0_0_12px] shadow-ember' : 'bg-fern'}`;
  $('#clip-text').textContent = st.clipping ? 'Limiting — lower the gain' : 'No clipping';
  $('#stream-url').textContent = st.stream_url || '—';

  // Meters
  st.inputs.forEach((inp, i) => {
    const pct = dbToPct(inp.level);
    $$(`[data-meter="${i}"]`).forEach(m => {
      m.style.width = `${pct}%`;
      m.style.setProperty('--meter-w', `${pct > 0 ? 10000 / pct : 100}%`);
    });
    $$(`[data-level="${i}"]`).forEach(el => { el.textContent = inp.level <= -119 ? '—' : `${inp.level.toFixed(0)} dB`; });
    $$(`[data-sig="${i}"]`).forEach(el => {
      el.textContent = inp.signal ? 'signal' : 'silent';
      el.className = `rounded-full px-2 py-0.5 text-[10px] font-bold uppercase ${inp.signal ? 'bg-lime/15 text-lime' : 'bg-fern/60 text-mist'}`;
    });
  });

  // Connect page
  const m = st.mqtt;
  $('#mqtt-dot').className = `size-2.5 rounded-full ${!m.enabled ? 'bg-mist/40' : m.connected ? 'bg-lime' : 'bg-sun animate-pulse'}`;
  $('#mqtt-state').textContent = !m.enabled ? 'Disabled' : m.connected ? `Connected · ${m.device_id}` : (m.error || 'Connecting…');
  $('#net-link').textContent = st.link ? `${st.speed} Mbit/s` : 'down';
  $('#net-ip').textContent = st.ip || '—';
  $('#net-gw').textContent = st.gateway || '—';
  $('#net-mac').textContent = st.mac;

  // System page
  const up = st.uptime_s, d = Math.floor(up / 86400), h = Math.floor(up / 3600) % 24, mi = Math.floor(up / 60) % 60;
  const rows = [
    ['Firmware', st.fw], ['ESP-IDF', st.idf], ['Uptime', `${d ? d + 'd ' : ''}${h}h ${mi}m`],
    ['Encoder', `${Math.round(st.bitrate / 1000)} kbit/s AAC`], ['Free RAM', `${Math.round(st.heap_free / 1024)} KB`],
    ['Free PSRAM', `${(st.psram_free / 1048576).toFixed(1)} MB`], ['Hostname', st.hostname],
  ];
  $('#sys-info').innerHTML = rows.map(([k, v]) => `<dt class="text-mist">${k}</dt><dd class="truncate font-mono text-xs leading-5">${esc(v)}</dd>`).join('');

  $('#banner-restart').hidden = !st.restart_required;
  $('#banner-fw').hidden = !state.fw || st.fw === state.fw;
}

/* --------------------------------------------------------------- Polling */

let pollTimer;
function schedulePoll(ms) {
  clearTimeout(pollTimer);
  pollTimer = setTimeout(poll, ms);
}

async function poll() {
  if (document.hidden) return schedulePoll(2000);
  try {
    const st = await api('GET', '/api/status');
    state.status = st;
    if (!state.fw) state.fw = st.fw;
    setOffline(false);
    // Settings changed elsewhere (MQTT, other browser): reload them
    if (st.settings_rev !== state.rev && !state.busy && !Object.keys(pending).length) {
      applySettings(await api('GET', '/api/settings'));
    } else {
      renderStatus();
    }
    const fast = ['live', 'inputs'].includes(currentTab());
    schedulePoll(fast ? 400 : 1500);
  } catch {
    setOffline(true);
    schedulePoll(2000);
  }
}

function setOffline(off) {
  state.offline = off;
  $('#banner-offline').hidden = !off;
  $('#conn-dot').className = `size-2.5 rounded-full ${off ? 'bg-ember' : 'bg-lime shadow-[0_0_10px] shadow-lime'}`;
}

async function waitForReboot(msg) {
  toast(msg);
  await new Promise(r => setTimeout(r, 4000));
  for (let i = 0; i < 60; i++) {
    try { await api('GET', '/api/status'); location.reload(); return; } catch { /* still down */ }
    await new Promise(r => setTimeout(r, 1500));
  }
}

/* ---------------------------------------------------------------- Events */

function bindEvents() {
  document.addEventListener('input', e => {
    const el = e.target;
    if (el.dataset.range) onRangeInput(el);
    const form = el.closest('form[data-form]');
    if (form) {
      state.dirtyForms.add(form.dataset.form);
      if (el.name === 'dhcp') updateStaticFields();
    }
  });

  document.addEventListener('pointerdown', e => { if (e.target.dataset?.range) state.busy++; });
  document.addEventListener('pointerup', () => { if (state.busy) setTimeout(() => { state.busy = 0; }, 400); });
  document.addEventListener('pointercancel', () => { state.busy = 0; });

  document.addEventListener('change', e => {
    const el = e.target, key = el.dataset.key;
    if (el.dataset.range) {
      const k = el.dataset.range;
      clearTimeout(pending[k]);
      delete pending[k];
      save({ [k]: rangeValue(el) }, true);
      return;
    }
    if (!key) return;
    if (el.type === 'checkbox') save({ [key]: el.checked });
    else if (el.tagName === 'SELECT') save({ [key]: el.value });
    else if (el.value.trim() !== String(state.settings[key])) save({ [key]: el.value.trim() });
  });

  document.addEventListener('keydown', e => {
    if (e.key === 'Enter' && e.target.dataset?.key && e.target.tagName === 'INPUT') e.target.blur();
  });

  document.addEventListener('click', async e => {
    const seg = e.target.closest('[data-seg]');
    if (seg) {
      $$(`[data-seg="${seg.dataset.seg}"]`).forEach(b => b.setAttribute('aria-pressed', b === seg));
      save({ [seg.dataset.seg]: seg.dataset.value }, true);
      return;
    }
    const band = e.target.closest('[data-band]');
    if (band) {
      const b = +band.dataset.band;
      state.band = state.band === b ? null : b;
      showBandDetail();
      drawEq();
      return;
    }
    if (e.target.closest('[data-band-close]')) {
      state.band = null;
      showBandDetail();
      drawEq();
      return;
    }
    const action = e.target.closest('[data-action]')?.dataset.action;
    if (action === 'eq-reset') {
      try { applySettings(await api('POST', '/api/eq/reset')); toast('Equalizer flat'); } catch (err) { toast(err.message, 'error'); }
    } else if (action === 'restart') {
      if (await ask({ title: 'Restart device?', text: 'The stream stops for a few seconds.', ok: 'Restart' })) {
        await api('POST', '/api/restart').catch(() => {});
        waitForReboot('Restarting…');
      }
    } else if (action === 'factory-reset') {
      if (await ask({ title: 'Factory reset', text: 'All settings will be erased, including MQTT and network. The device restarts with DHCP.', ok: 'Erase everything', danger: true, confirmWord: 'RESET' })) {
        await api('POST', '/api/factory_reset', { confirm: 'RESET' }).catch(() => {});
        waitForReboot('Resetting…');
      }
    }
  });

  $('#btn-copy').addEventListener('click', () => copyText($('#stream-url').textContent));

  $('form[data-form="mqtt"]').addEventListener('submit', async e => {
    e.preventDefault();
    const form = e.target, patch = {};
    $$('[name]', form).forEach(el => {
      if (el.type === 'checkbox') patch[el.name] = el.checked;
      else if (el.name === 'mqtt_password') { if (el.value) patch[el.name] = el.value; }
      else patch[el.name] = el.value.trim();
    });
    if (await save(patch)) {
      state.dirtyForms.delete('mqtt');
      form.mqtt_password.value = '';
      render();
    }
  });

  $('form[data-form="network"]').addEventListener('submit', async e => {
    e.preventDefault();
    const form = e.target, patch = {};
    $$('[name]', form).forEach(el => { patch[el.name] = el.type === 'checkbox' ? el.checked : el.value.trim(); });
    const where = patch.dhcp ? `http://${patch.hostname}.local/ or the address your router assigns`
                             : `http://${patch.ip}/`;
    if (!await ask({ title: 'Apply network settings?', text: `The device restarts. Afterwards it is reachable at ${where}.`, ok: 'Save & restart' })) return;
    if (await save(patch)) {
      state.dirtyForms.delete('network');
      await api('POST', '/api/restart').catch(() => {});
      if (!patch.dhcp && patch.ip !== state.status?.ip) {
        toast(`Restarting — open http://${patch.ip}/`);
      } else {
        waitForReboot('Restarting…');
      }
    }
  });

  // Firmware upload
  const file = $('#ota-file');
  file.addEventListener('change', () => {
    $('#ota-name').textContent = file.files[0] ? `${file.files[0].name} · ${(file.files[0].size / 1048576).toFixed(2)} MB` : 'Choose firmware file…';
    $('#btn-ota').disabled = !file.files[0];
  });
  $('#btn-ota').addEventListener('click', async () => {
    const f = file.files[0];
    if (!f || !await ask({ title: 'Update firmware?', text: `Upload ${f.name} and restart. The stream stops for about a minute.`, ok: 'Update' })) return;
    const bar = $('#ota-progress'), fill = $('.meter-fill', bar);
    bar.hidden = false;
    $('#btn-ota').disabled = true;
    const xhr = new XMLHttpRequest();
    xhr.open('POST', '/api/ota');
    xhr.upload.onprogress = ev => { if (ev.lengthComputable) fill.style.width = `${ev.loaded / ev.total * 100}%`; };
    xhr.onload = () => {
      let msg = '';
      try { msg = JSON.parse(xhr.responseText).error || ''; } catch { /* empty */ }
      if (xhr.status === 200) {
        state.fw = state.status?.fw;
        waitForReboot('Update installed, restarting…');
      } else {
        toast(msg || `Upload failed (${xhr.status})`, 'error');
        bar.hidden = true;
        $('#btn-ota').disabled = false;
      }
    };
    xhr.onerror = () => { toast('Upload failed', 'error'); bar.hidden = true; $('#btn-ota').disabled = false; };
    xhr.send(f);
  });

  document.addEventListener('visibilitychange', () => { if (!document.hidden) schedulePoll(0); });
}

function copyText(text) {
  const done = () => toast('Copied');
  if (navigator.clipboard && window.isSecureContext) {
    navigator.clipboard.writeText(text).then(done, () => {});
    return;
  }
  // Plain HTTP: the clipboard API is not available
  const ta = document.createElement('textarea');
  ta.value = text;
  ta.style.position = 'fixed';
  ta.style.opacity = '0';
  document.body.appendChild(ta);
  ta.select();
  try { document.execCommand('copy'); done(); } catch { /* ignore */ }
  ta.remove();
}

/* ------------------------------------------------------------------ Boot */

async function boot() {
  buildTabs();
  bindEvents();
  for (;;) {
    try {
      const data = await api('GET', '/api/settings');
      state.fields = data.fields;
      state.settings = data.settings;
      state.bands = data.eq_bands;
      buildControls();
      applySettings(data);
      break;
    } catch {
      setOffline(true);
      await new Promise(r => setTimeout(r, 2000));
    }
  }
  schedulePoll(0);
}

boot();
