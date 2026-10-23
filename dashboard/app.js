/* =====================================================================
   Synaptix Robotics — Motor Health Monitor (dashboard client)
   Talks to the ESP8266 firmware:  GET /api/status | /api/relay | /api/reset
                                   GET|POST /api/config
   No build step, no dependencies.
   ===================================================================== */
'use strict';

/* ------------------------------ constants --------------------------- */
const POLL_MS   = 1000;
const MAX_POINTS = 120;              // 2 minutes at 1 Hz
const LS_KEY     = 'synaptix.pdm.ip';

const COLOR = {
  series:  '#2a78d6',
  fill:    'rgba(42,120,214,0.10)',
  grid:    '#e8ebf1',
  axis:    '#c3c2b7',
  muted:   '#898781',
  ink:     '#0b0b0b',
  good:    '#0ca30c',
  warning: '#fab219',
  serious: '#ec835a',
  critical:'#d03b3b',
  surface: '#ffffff'
};

const CLASSES = [
  { name: 'Healthy',                 short: 'Healthy',      sev: 0 },
  { name: 'Overvoltage Stress',      short: 'Overvoltage',  sev: 3 },
  { name: 'Undervoltage / Brownout', short: 'Undervoltage', sev: 2 },
  { name: 'Mechanical Overload',     short: 'Overload',     sev: 3 },
  { name: 'Thermal Overheat',        short: 'Overheat',     sev: 3 },
  { name: 'Bearing Wear',            short: 'Bearing wear', sev: 2 },
  { name: 'Rotor Imbalance',         short: 'Imbalance',    sev: 2 }
];

/* The same suggestion text the firmware serves, mirrored here so demo
   mode and a stale connection still give the operator guidance.       */
const ADVICE = [
  { lead: 'No action required. The motor is inside every limit.',
    steps: ['Keep logging — the model needs running hours to refine the wear trend.',
            'Next scheduled lubrication in roughly 50 running hours.'] },
  { lead: 'STOP — supply voltage is above the 6 V motor rating. The relay has opened automatically.',
    steps: ['Lower the bench-supply setpoint to 5–6 V before re-energising.',
            'Check the buck-converter feedback divider and any series resistor.',
            'Confirm the voltage sensor calibration, then press Reset latch.'] },
  { lead: 'Supply has sagged below the safe minimum. Sustained brownout drives high slip current and winding heat.',
    steps: ['Reduce the mechanical load or recharge/replace the source.',
            'Check wire gauge and connector voltage drop to the motor.',
            'Verify the battery or adapter can deliver the stall current.'] },
  { lead: 'STOP — current is above the overload limit, consistent with a stall or jam. The relay has opened.',
    steps: ['Power down and turn the shaft by hand to find the obstruction.',
            'Remove excess load; check for a seized coupling or blocked fan.',
            'Inspect the gearbox and re-check free-running current, then Reset latch.'] },
  { lead: 'STOP — winding temperature is over the limit. The relay has opened.',
    steps: ['Let the motor cool below 40 °C before restarting.',
            'Improve ventilation and confirm the cooling fan is turning.',
            'Review the duty cycle — continuous running may exceed the rating.'] },
  { lead: 'Schedule maintenance: the vibration signature matches bearing wear.',
    steps: ['Re-lubricate or replace the bearing within the next 20 running hours.',
            'Inspect for axial play and listen for a dry rumble while coasting.',
            'Re-measure vibration after service to confirm the trend reverses.'] },
  { lead: 'Inspect the rotor and coupling: the vibration pattern indicates imbalance.',
    steps: ['Re-seat the fan or flywheel and tighten the set-screw.',
            'Check for a bent shaft or a chipped blade; re-balance the load.',
            'Torque the mount bolts — a loose mount mimics imbalance.'] }
];

const METRICS = {
  voltage:     { label: 'Supply voltage',  unit: 'V',    dec: 2, title: 'Supply voltage',  sub: 'volts, 1 Hz sampling' },
  current:     { label: 'Motor current',   unit: 'A',    dec: 2, title: 'Motor current',   sub: 'amperes, 1 Hz sampling' },
  temperature: { label: 'Winding temp',    unit: '°C',   dec: 1, title: 'Winding temperature', sub: 'degrees Celsius, DHT11' },
  vibration:   { label: 'Vibration rate',  unit: 'ev/s', dec: 0, title: 'Vibration rate',  sub: 'events per second, 1 Hz sampling' }
};

/* ------------------------------ state ------------------------------- */
const S = {
  ip: '',
  mode: 'idle',                 // idle | live | demo | error
  metric: 'vibration',
  series: { voltage: [], current: [], temperature: [], vibration: [] },
  classes: [],
  last: null,
  thresholds: { voltage_max: 6, voltage_min: 3.5, current_max: 1.2,
                temp_warn: 45, temp_max: 55, vib_warn: 18, vib_max: 35 },
  timer: null,
  hover: -1,
  fails: 0,
  demo: { inject: null, t: 0, latched: false, relayOn: true, trips: 0, runS: 0 }
};

const $  = (s, r = document) => r.querySelector(s);
const $$ = (s, r = document) => [...r.querySelectorAll(s)];
const clamp = (v, a, b) => Math.min(b, Math.max(a, v));
const fmt = (v, d) => (v === null || v === undefined || Number.isNaN(v) ? '—' : Number(v).toFixed(d));

/* ------------------------------ logging ----------------------------- */
function log(msg, kind = 'info') {
  const ol = $('#log');
  const li = document.createElement('li');
  li.dataset.k = kind;
  const t = new Date().toLocaleTimeString([], { hour12: false });
  li.innerHTML = `<time>${t}</time><span></span>`;
  li.lastChild.textContent = msg;
  ol.prepend(li);
  while (ol.children.length > 120) ol.lastChild.remove();
}

/* ------------------------------ connection -------------------------- */
function normaliseHost(raw) {
  let h = (raw || '').trim().replace(/\/+$/, '');
  if (!h) return '';
  if (!/^https?:\/\//i.test(h)) h = 'http://' + h;
  return h;
}

function setMode(mode, text) {
  S.mode = mode;
  const pill = $('#statusPill');
  pill.className = 'pill ' + ({ idle: 'pill-idle', live: 'pill-live',
                                demo: 'pill-demo', error: 'pill-err' }[mode]);
  $('#statusText').textContent = text;
  $('#demoBtn').setAttribute('aria-pressed', String(mode === 'demo'));
  $('#demoTools').hidden = mode !== 'demo';
}

function showBanner(html) {
  const b = $('#banner');
  if (!html) { b.hidden = true; b.innerHTML = ''; return; }
  b.innerHTML = html;
  b.hidden = false;
}

/* An HTTPS page cannot call the ESP8266's HTTP API. We only say so once
   requests have actually failed: a browser set to allow insecure content
   works fine, and a warning over a working connection is just noise. */
function mixedContentWarn() {
  if (location.protocol === 'https:') {
    showBanner(
      '<strong>Requests are being blocked.</strong> This page is served over ' +
      'HTTPS, but the ESP8266 only serves HTTP. Open <code>dashboard-local.html</code> ' +
      'directly from disk, or allow insecure content for this site in the browser ' +
      'settings. <strong>Demo mode works everywhere.</strong>'
    );
  }
}

async function fetchJson(url, opts = {}, ms = 2500) {
  const ac = new AbortController();
  const id = setTimeout(() => ac.abort(), ms);
  try {
    const r = await fetch(url, { ...opts, signal: ac.signal, cache: 'no-store' });
    if (!r.ok) throw new Error('HTTP ' + r.status);
    return await r.json();
  } finally { clearTimeout(id); }
}

function connect() {
  const host = normaliseHost($('#ipInput').value);
  if (!host) { log('Enter the ESP8266 IP address first.', 'warn'); return; }
  S.ip = host;
  localStorage.setItem(LS_KEY, $('#ipInput').value.trim());
  stopPolling();
  S.fails = 0;
  setMode('live', 'Connecting…');
  log(`Connecting to ${host} …`);
  S.timer = setInterval(pollLive, POLL_MS);
  pollLive();
  loadDeviceConfig();
}

function stopPolling() { if (S.timer) { clearInterval(S.timer); S.timer = null; } }

async function loadDeviceConfig() {
  try {
    const c = await fetchJson(S.ip + '/api/config');
    Object.assign(S.thresholds, c);
    fillThresholdForm();
  } catch { /* non-fatal */ }
}

async function pollLive() {
  try {
    const d = await fetchJson(S.ip + '/api/status');
    S.fails = 0;
    showBanner(null);
    if (S.mode !== 'live') setMode('live', 'Live');
    ingestDevice(d);
  } catch (e) {
    if (++S.fails === 1) log('No response from device: ' + e.message, 'warn');
    if (S.fails >= 3) {
      setMode('error', 'Device unreachable');
      $('#statusText').textContent = 'Device unreachable';
      mixedContentWarn();
    }
  }
}

/* ------------------------------ ingest ------------------------------ */
function pushPoint(v, i, t, vib, cls) {
  const s = S.series;
  s.voltage.push(v); s.current.push(i); s.temperature.push(t); s.vibration.push(vib);
  S.classes.push(cls);
  for (const k in s) if (s[k].length > MAX_POINTS) s[k].shift();
  if (S.classes.length > MAX_POINTS) S.classes.shift();
}

function ingestDevice(d) {
  const s = d.sensors || {}, m = d.ml || {}, p = d.protection || {};

  /* first payload: adopt the device history so the chart is not empty */
  if (Array.isArray(d.history) && d.history.length && S.series.voltage.length === 0) {
    for (const [v, i, t, vib, c] of d.history) pushPoint(v, i, t, vib, c);
  } else {
    pushPoint(s.voltage, s.current, s.temperature, s.vibration, m.class_id ?? 0);
  }

  if (d.thresholds) { Object.assign(S.thresholds, d.thresholds); fillThresholdForm(); }

  const prev = S.last;
  S.last = {
    sensors: s,
    cls: m.class_id ?? 0,
    confidence: m.confidence ?? 0,
    severity: m.severity ?? 0,
    suggestion: m.suggestion || '',
    rul: m.rul_hours ?? 0,
    model: m.model || '—',
    relayOn: !!p.relay_on,
    latched: !!p.latched,
    trips: p.trip_count ?? 0,
    runS: d.run_seconds ?? 0,
    meta: (() => {
      const g = d.diagnostics || {};
      const bits = [
        d.device || 'device',
        d.ip || S.ip,
        `RSSI ${d.rssi ?? '—'} dBm`,
        `up ${Math.floor((d.uptime_s || 0) / 60)} min`
      ];
      if (g.i2c_bus_fault) bits.push('I²C BUS FAULT');
      else if (Array.isArray(g.i2c_devices))
        bits.push(`I²C: ${g.i2c_devices.length ? g.i2c_devices.join(', ') : 'none'}`);
      if (g.sensor_ok === false) bits.push(`${g.sensor || 'power sensor'} not detected`);
      return bits.join(' · ');
    })()
  };

  if (prev && prev.cls !== S.last.cls)
    log(`ML: ${CLASSES[S.last.cls].name} (${Math.round(S.last.confidence*100)}% confidence)`,
        S.last.severity >= 3 ? 'trip' : S.last.severity === 2 ? 'warn' : 'ok');
  if (prev && !prev.latched && S.last.latched)
    log(`RELAY OPENED — ${p.last_trip || 'protection trip'}`, 'trip');
  if (prev && prev.latched && !S.last.latched)
    log('Latch cleared, motor re-energised.', 'ok');

  render();
}

/* ------------------------------ demo mode --------------------------- */
function demoTick() {
  const D = S.demo;
  D.t += 1;
  const wob = (a) => (Math.random() - 0.5) * a;
  let v = 5.1 + wob(0.12), i = 0.42 + wob(0.08), t = 32 + Math.sin(D.t / 40) * 2.2 + wob(0.3),
      vib = 6 + Math.abs(Math.sin(D.t / 11)) * 5 + wob(2);

  switch (D.inject) {
    case 'overvoltage': v = 7.4 + wob(0.3); i = 0.72 + wob(0.1); break;
    case 'overheat':    t = 61 + wob(1.5); break;
    case 'bearing':     vib = 24 + wob(5); break;
    case 'overload':    i = 1.75 + wob(0.2); v = 4.8 + wob(0.1); break;
  }
  vib = Math.max(0, vib);

  /* same classification order the firmware's tree uses */
  let cls = 0;
  if (v > S.thresholds.voltage_max)       cls = 1;
  else if (t > S.thresholds.temp_max)     cls = 4;
  else if (i > S.thresholds.current_max)  cls = 3;
  else if (vib > S.thresholds.vib_max)    cls = 6;
  else if (vib > S.thresholds.vib_warn)   cls = 5;
  else if (v < S.thresholds.voltage_min)  cls = 2;
  else if (t > S.thresholds.temp_warn)    cls = 4;

  const sev = CLASSES[cls].sev;
  const prevCls = S.last ? S.last.cls : null;
  const prevLatched = D.latched;

  if (sev >= 3 && !D.latched) { D.latched = true; D.relayOn = false; D.trips++; }
  if (D.relayOn) D.runS++;

  const conf = cls === 0 ? 0.981 : 0.86 + Math.random() * 0.12;
  pushPoint(+v.toFixed(2), +i.toFixed(2), +t.toFixed(1), Math.round(vib), cls);

  S.last = {
    sensors: { voltage: +v.toFixed(2), current: +i.toFixed(2),
               temperature: +t.toFixed(1), humidity: 48 + Math.round(wob(4)),
               vibration: Math.round(vib), power: +(v * i).toFixed(2),
               current_sensor_ok: true },
    cls, confidence: conf, severity: sev,
    suggestion: '', rul: cls === 0 ? 420 : cls === 5 ? 20 : cls === 6 ? 35 : 0,
    model: 'dt-v1.3.0 (demo)',
    relayOn: D.relayOn, latched: D.latched, trips: D.trips, runS: D.runS,
    meta: 'Demo simulator · no hardware attached'
  };

  if (prevCls !== null && prevCls !== cls)
    log(`ML: ${CLASSES[cls].name} (${Math.round(conf*100)}% confidence)`,
        sev >= 3 ? 'trip' : sev === 2 ? 'warn' : 'ok');
  if (!prevLatched && D.latched) log(`RELAY OPENED — ${CLASSES[cls].short.toUpperCase()}`, 'trip');

  render();
}

function startDemo() {
  stopPolling();
  S.ip = '';
  S.series = { voltage: [], current: [], temperature: [], vibration: [] };
  S.classes = [];
  S.demo = { inject: null, t: 0, latched: false, relayOn: true, trips: 0, runS: 0 };
  S.last = null;
  showBanner(null);
  setMode('demo', 'Demo mode');
  log('Demo mode started — synthetic telemetry, no hardware needed.');
  S.timer = setInterval(demoTick, POLL_MS);
  demoTick();
}

/* ------------------------------ controls ---------------------------- */
async function deviceCall(path, okMsg) {
  if (S.mode === 'demo') return;
  if (!S.ip) { log('Not connected to a device.', 'warn'); return; }
  try { await fetchJson(S.ip + path); log(okMsg, 'ok'); }
  catch (e) { log('Command failed: ' + e.message, 'warn'); }
}

function resetLatch() {
  if (S.mode === 'demo') {
    S.demo.inject = null; S.demo.latched = false; S.demo.relayOn = true;
    log('Latch cleared (demo), motor re-energised.', 'ok');
    return;
  }
  deviceCall('/api/reset', 'Reset latch command sent.');
}

function setRelay(on) {
  if (S.mode === 'demo') {
    if (on && S.demo.latched) { log('Cannot start: fault latch is set.', 'warn'); return; }
    S.demo.relayOn = on;
    log(on ? 'Motor started (demo).' : 'Motor stopped (demo).', on ? 'ok' : 'warn');
    return;
  }
  deviceCall('/api/relay?state=' + (on ? 'on' : 'off'),
             on ? 'Start command sent.' : 'Stop command sent.');
}

async function applyThresholds(ev) {
  ev.preventDefault();
  const body = {};
  for (const el of $$('#thForm input')) body[el.name] = parseFloat(el.value);
  Object.assign(S.thresholds, body);
  if (S.mode === 'demo') { log('Thresholds applied locally (demo).', 'ok'); render(); return; }
  if (!S.ip) { log('Not connected — thresholds kept locally only.', 'warn'); render(); return; }
  try {
    await fetchJson(S.ip + '/api/config',
      { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) });
    log('Thresholds written to the device.', 'ok');
  } catch (e) { log('Could not write thresholds: ' + e.message, 'warn'); }
  render();
}

function fillThresholdForm() {
  for (const el of $$('#thForm input'))
    if (S.thresholds[el.name] !== undefined && document.activeElement !== el)
      el.value = S.thresholds[el.name];
}

/* ------------------------------ render ------------------------------ */
function stateOf(metric, value) {
  const T = S.thresholds;
  if (value === null || value === undefined || Number.isNaN(value)) return 'na';
  switch (metric) {
    case 'voltage':     return value > T.voltage_max ? 'crit'
                             : value < T.voltage_min ? 'warn'
                             : value > T.voltage_max - 0.4 ? 'warn' : 'ok';
    case 'current':     return value > T.current_max ? 'crit'
                             : value > T.current_max * 0.85 ? 'warn' : 'ok';
    case 'temperature': return value > T.temp_max ? 'crit'
                             : value > T.temp_warn ? 'warn' : 'ok';
    case 'vibration':   return value > T.vib_max ? 'crit'
                             : value > T.vib_warn ? 'warn' : 'ok';
  }
  return 'ok';
}

const CHIP = { ok: ['chip-good', 'Normal'], warn: ['chip-warn', 'Watch'],
               crit: ['chip-crit', 'Over limit'], na: ['chip-good', 'No data'] };

function render() {
  const L = S.last;
  if (!L) return;
  const s = L.sensors;

  /* verdict */
  const C = CLASSES[L.cls] || CLASSES[0];
  $('#verdict').dataset.sev = L.severity;
  $('#faultName').textContent = C.name;
  $('#sevIcon').textContent = L.severity >= 3 ? '✕' : L.severity === 2 ? '!' : '✓';
  const chip = $('#sevChip');
  chip.className = 'chip ' + (L.severity >= 3 ? 'chip-crit' : L.severity === 2 ? 'chip-warn' : 'chip-good');
  chip.textContent = L.severity >= 3 ? 'Trip — motor stopped' : L.severity === 2 ? 'Maintenance due' : 'Normal';
  $('#confVal').textContent = Math.round(L.confidence * 100) + '%';
  $('#confFill').style.width = Math.round(L.confidence * 100) + '%';
  $('#modelVer').textContent = 'model ' + L.model;

  $('#relayState').textContent = L.relayOn ? 'Closed — running' : 'Open — stopped';
  $('#latchState').textContent = L.latched ? 'Latched' : 'Clear';
  $('#rulState').textContent = L.rul ? L.rul + ' h' : 'Service now';
  $('#tripCount').textContent = L.trips;
  $('#deviceMeta').textContent = L.meta;

  /* advice */
  const a = ADVICE[L.cls] || ADVICE[0];
  $('#adviceText').textContent = L.suggestion || a.lead;
  const ul = $('#adviceList');
  ul.innerHTML = '';
  for (const step of a.steps) {
    const li = document.createElement('li');
    li.textContent = step;
    ul.appendChild(li);
  }
  $('#adviceCard').style.borderLeftColor =
    L.severity >= 3 ? COLOR.critical : L.severity === 2 ? COLOR.warning : COLOR.series;

  /* tiles */
  const limits = { voltage: S.thresholds.voltage_max, current: S.thresholds.current_max,
                   temperature: S.thresholds.temp_max, vibration: S.thresholds.vib_max };
  for (const key of Object.keys(METRICS)) {
    const tile = $('#tile-' + key);
    const def  = METRICS[key];
    const val  = s[key === 'vibration' ? 'vibration' : key];
    const missing = (key === 'current'  && s.current_sensor_ok === false) ||
                    (key === 'voltage'  && s.voltage_sensor_ok === false);
    const st   = missing ? 'na' : stateOf(key, val);
    tile.dataset.state = st === 'ok' || st === 'na' ? '' : st;
    $('[data-val]', tile).textContent =
      (st === 'na') ? 'n/a' : fmt(val, def.dec);
    $('[data-limit]', tile).textContent = fmt(limits[key], def.dec);
    const c = $('[data-chip]', tile);
    c.className = 'chip ' + CHIP[st][0];
    c.textContent = st === 'na' ? 'No sensor' : CHIP[st][1];
    drawSpark($('[data-spark]', tile), S.series[key], st);
  }

  drawMainChart();
  drawTimeline();
  renderTable();
  fillThresholdForm();
}

/* ------------------------------ canvas helpers ---------------------- */
function prep(cv) {
  const dpr = window.devicePixelRatio || 1;
  const w = cv.clientWidth || cv.width, h = cv.clientHeight || cv.height;
  if (cv.width !== Math.round(w * dpr) || cv.height !== Math.round(h * dpr)) {
    cv.width = Math.round(w * dpr); cv.height = Math.round(h * dpr);
  }
  const ctx = cv.getContext('2d');
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, w, h);
  return { ctx, w, h };
}

function niceStep(span, target) {
  const raw = span / target;
  const mag = Math.pow(10, Math.floor(Math.log10(raw || 1)));
  const n = raw / mag;
  return (n <= 1 ? 1 : n <= 2 ? 2 : n <= 5 ? 5 : 10) * mag;
}

/* ------------------------------ sparkline --------------------------- */
function drawSpark(cv, data, state) {
  const { ctx, w, h } = prep(cv);
  if (!data.length) return;
  const stroke = state === 'crit' ? COLOR.critical : state === 'warn' ? COLOR.serious : COLOR.series;
  const pad = 6;                     // room for the end marker's stroke
  const lo = Math.min(...data), hi = Math.max(...data);
  const span = (hi - lo) || 1;
  const X = k => pad + (k / Math.max(1, data.length - 1)) * (w - pad * 2);
  const Y = v => h - pad - ((v - lo) / span) * (h - pad * 2);

  ctx.beginPath();
  ctx.moveTo(X(0), h);
  data.forEach((v, k) => ctx.lineTo(X(k), Y(v)));
  ctx.lineTo(X(data.length - 1), h);
  ctx.closePath();
  ctx.fillStyle = state === 'ok' ? COLOR.fill : 'rgba(208,59,59,0.08)';
  ctx.fill();

  ctx.beginPath();
  data.forEach((v, k) => k ? ctx.lineTo(X(k), Y(v)) : ctx.moveTo(X(k), Y(v)));
  ctx.lineWidth = 2; ctx.lineJoin = 'round'; ctx.strokeStyle = stroke;
  ctx.stroke();

  /* 4px rounded data-end marker */
  const lx = X(data.length - 1), ly = Y(data[data.length - 1]);
  ctx.beginPath(); ctx.arc(lx, ly, 3, 0, Math.PI * 2);
  ctx.fillStyle = COLOR.surface; ctx.fill();
  ctx.lineWidth = 2; ctx.strokeStyle = stroke; ctx.stroke();
}

/* ------------------------------ main chart -------------------------- */
function drawMainChart() {
  const cv = $('#mainChart');
  const { ctx, w, h } = prep(cv);
  const data = S.series[S.metric];
  const def = METRICS[S.metric];
  const padL = 46, padR = 14, padT = 12, padB = 26;
  const pw = w - padL - padR, ph = h - padT - padB;

  if (!data.length) {
    ctx.fillStyle = COLOR.muted; ctx.font = '13px system-ui';
    ctx.textAlign = 'center';
    ctx.fillText('Waiting for telemetry…', w / 2, h / 2);
    return;
  }

  /* y domain includes the relevant threshold so the limit line is visible */
  const limit = { voltage: S.thresholds.voltage_max, current: S.thresholds.current_max,
                  temperature: S.thresholds.temp_max, vibration: S.thresholds.vib_max }[S.metric];
  let lo = Math.min(...data, limit), hi = Math.max(...data, limit);
  if (hi - lo < 1e-6) { hi += 1; lo -= 1; }
  const padv = (hi - lo) * 0.12;
  lo = Math.max(0, lo - padv); hi = hi + padv;

  /* right-align: the newest sample always sits on the right edge, so a
     partially filled window fills from the right instead of the left */
  const off = MAX_POINTS - data.length;
  const X = k => padL + ((k + off) / Math.max(1, MAX_POINTS - 1)) * pw;
  const Y = v => padT + ph - ((v - lo) / (hi - lo)) * ph;

  /* grid + y ticks */
  const step = niceStep(hi - lo, 4);
  ctx.font = '11px system-ui'; ctx.textBaseline = 'middle';
  for (let v = Math.ceil(lo / step) * step; v <= hi; v += step) {
    const y = Math.round(Y(v)) + .5;
    ctx.beginPath(); ctx.moveTo(padL, y); ctx.lineTo(w - padR, y);
    ctx.lineWidth = 1; ctx.strokeStyle = COLOR.grid; ctx.stroke();
    ctx.fillStyle = COLOR.muted; ctx.textAlign = 'right';
    ctx.fillText(v.toFixed(def.dec), padL - 8, y);
  }

  /* threshold line */
  const ly = Math.round(Y(limit)) + .5;
  if (ly > padT && ly < padT + ph) {
    ctx.save();
    ctx.setLineDash([5, 4]);
    ctx.beginPath(); ctx.moveTo(padL, ly); ctx.lineTo(w - padR, ly);
    ctx.lineWidth = 1.5; ctx.strokeStyle = COLOR.critical; ctx.stroke();
    ctx.restore();
    ctx.fillStyle = COLOR.critical; ctx.textAlign = 'left';
    ctx.font = '600 11px system-ui';
    ctx.fillText('trip limit ' + limit, padL + 6, ly - 9);
  }

  /* area + line */
  ctx.beginPath();
  ctx.moveTo(X(0), padT + ph);
  data.forEach((v, k) => ctx.lineTo(X(k), Y(v)));
  ctx.lineTo(X(data.length - 1), padT + ph);
  ctx.closePath();
  ctx.fillStyle = COLOR.fill; ctx.fill();

  ctx.beginPath();
  data.forEach((v, k) => k ? ctx.lineTo(X(k), Y(v)) : ctx.moveTo(X(k), Y(v)));
  ctx.lineWidth = 2; ctx.lineJoin = 'round'; ctx.strokeStyle = COLOR.series;
  ctx.stroke();

  /* baseline */
  ctx.beginPath();
  ctx.moveTo(padL, padT + ph + .5); ctx.lineTo(w - padR, padT + ph + .5);
  ctx.lineWidth = 1; ctx.strokeStyle = COLOR.axis; ctx.stroke();

  /* x labels: seconds ago */
  ctx.fillStyle = COLOR.muted; ctx.font = '11px system-ui'; ctx.textAlign = 'center';
  ctx.textBaseline = 'top';
  const n = data.length;
  [0, Math.floor(n / 2), n - 1].forEach(k => {
    const ago = n - 1 - k;
    ctx.fillText(ago === 0 ? 'now' : `-${ago}s`, X(k), padT + ph + 7);
  });

  /* last-value direct label (never a label on every point) */
  const lastX = X(n - 1), lastY = Y(data[n - 1]);
  ctx.beginPath(); ctx.arc(lastX, lastY, 4, 0, Math.PI * 2);
  ctx.fillStyle = COLOR.surface; ctx.fill();
  ctx.lineWidth = 2; ctx.strokeStyle = COLOR.series; ctx.stroke();
  const lbl = fmt(data[n - 1], def.dec) + ' ' + def.unit;
  ctx.font = '600 12px system-ui'; ctx.textAlign = 'right'; ctx.textBaseline = 'bottom';
  ctx.fillStyle = COLOR.ink;
  ctx.fillText(lbl, Math.min(lastX - 8, w - padR), lastY - 6);

  /* crosshair */
  if (S.hover >= 0 && S.hover < n) {
    const hx = X(S.hover), hy = Y(data[S.hover]);
    ctx.save(); ctx.setLineDash([3, 3]);
    ctx.beginPath(); ctx.moveTo(hx, padT); ctx.lineTo(hx, padT + ph);
    ctx.lineWidth = 1; ctx.strokeStyle = COLOR.axis; ctx.stroke(); ctx.restore();
    ctx.beginPath(); ctx.arc(hx, hy, 5, 0, Math.PI * 2);
    ctx.fillStyle = COLOR.series; ctx.fill();
    ctx.lineWidth = 2; ctx.strokeStyle = COLOR.surface; ctx.stroke();
  }
}

/* ------------------------------ fault timeline ---------------------- */
const SEV_COLOR = { 0: COLOR.good, 2: COLOR.warning, 3: COLOR.critical };

function drawTimeline() {
  const cv = $('#timeline');
  const { ctx, w, h } = prep(cv);
  const n = S.classes.length;
  if (!n) return;
  const bw = w / MAX_POINTS;
  const off = MAX_POINTS - n;                 // newest band on the right
  S.classes.forEach((c, k) => {
    const sev = (CLASSES[c] || CLASSES[0]).sev;
    ctx.fillStyle = SEV_COLOR[sev] || COLOR.good;
    /* 2px surface gap between adjacent bands */
    ctx.fillRect((k + off) * bw, 4, Math.max(1, bw - 2), h - 8);
  });
  const legend = $('#timelineLegend');
  if (!legend.childElementCount) {
    const seen = [['Healthy', COLOR.good, '✓'], ['Maintenance due', COLOR.warning, '!'],
                  ['Trip / stop', COLOR.critical, '✕']];
    for (const [name, col, icon] of seen) {
      const li = document.createElement('li');
      li.innerHTML = `<i style="background:${col}"></i><span>${icon} ${name}</span>`;
      legend.appendChild(li);
    }
  }
}

/* ------------------------------ table ------------------------------- */
function renderTable() {
  const tb = $('#dataTable tbody');
  const n = S.series.voltage.length;
  const rows = [];
  for (let k = n - 1; k >= Math.max(0, n - 30); k--) {
    const cls = CLASSES[S.classes[k]] || CLASSES[0];
    rows.push(`<tr><td>-${n - 1 - k}s</td><td>${fmt(S.series.voltage[k],2)}</td>` +
              `<td>${fmt(S.series.current[k],2)}</td><td>${fmt(S.series.temperature[k],1)}</td>` +
              `<td>${fmt(S.series.vibration[k],0)}</td><td>${cls.short}</td></tr>`);
  }
  tb.innerHTML = rows.join('');
}

/* ------------------------------ events ------------------------------ */
$('#connectBtn').addEventListener('click', connect);
$('#ipInput').addEventListener('keydown', e => { if (e.key === 'Enter') connect(); });
$('#demoBtn').addEventListener('click', () => {
  if (S.mode === 'demo') { stopPolling(); setMode('idle', 'Not connected'); log('Demo mode stopped.'); }
  else startDemo();
});
$('#resetBtn').addEventListener('click', resetLatch);
$('#stopBtn').addEventListener('click', () => setRelay(false));
$('#startBtn').addEventListener('click', () => setRelay(true));
$('#thForm').addEventListener('submit', applyThresholds);

$$('.seg button').forEach(b => b.addEventListener('click', () => {
  S.metric = b.dataset.metric;
  $$('.seg button').forEach(x => x.setAttribute('aria-selected', String(x === b)));
  const d = METRICS[S.metric];
  $('#chartTitle').textContent = `${d.title} — last 2 minutes`;
  $('#chartSub').textContent = d.sub;
  drawMainChart();
}));

$$('[data-inject]').forEach(b => b.addEventListener('click', () => {
  const kind = b.dataset.inject;
  S.demo.inject = kind === 'clear' ? null : kind;
  if (kind === 'clear') { S.demo.latched = false; S.demo.relayOn = true; }
  log(kind === 'clear' ? 'Fault injection cleared.' : `Injecting ${kind} fault…`,
      kind === 'clear' ? 'ok' : 'warn');
}));

/* crosshair tooltip on the main chart */
const mainCv = $('#mainChart'), tip = $('#tooltip');
function hoverFromEvent(ev) {
  const r = mainCv.getBoundingClientRect();
  const x = ev.clientX - r.left;
  const padL = 46, padR = 14;
  const pw = r.width - padL - padR;
  const n = S.series[S.metric].length;
  if (!n) return -1;
  const off = MAX_POINTS - n;
  const k = Math.round(((x - padL) / pw) * (MAX_POINTS - 1)) - off;
  return (k < 0 || k >= n) ? -1 : k;
}
mainCv.addEventListener('mousemove', ev => {
  const k = hoverFromEvent(ev);
  S.hover = k;
  if (k < 0) { tip.hidden = true; drawMainChart(); return; }
  const def = METRICS[S.metric];
  const n = S.series[S.metric].length;
  const cls = CLASSES[S.classes[k]] || CLASSES[0];
  tip.innerHTML = `<b>${fmt(S.series[S.metric][k], def.dec)} ${def.unit}</b><br>` +
                  `<span style="color:#898781">${n - 1 - k === 0 ? 'now' : (n - 1 - k) + 's ago'} · ${cls.short}</span>`;
  const r = mainCv.getBoundingClientRect();
  tip.style.left = (ev.clientX - r.left) + 'px';
  tip.style.top  = Math.max(34, ev.clientY - r.top) + 'px';
  tip.hidden = false;
  drawMainChart();
});
mainCv.addEventListener('mouseleave', () => { S.hover = -1; tip.hidden = true; drawMainChart(); });

let rz;
window.addEventListener('resize', () => {
  clearTimeout(rz);
  rz = setTimeout(() => { if (S.last) render(); }, 120);
});

/* ------------------------------ boot -------------------------------- */
(function boot() {
  const saved = localStorage.getItem(LS_KEY);
  if (saved) $('#ipInput').value = saved;
  setMode('idle', 'Not connected');
  fillThresholdForm();
  log('Dashboard ready. Enter the ESP8266 IP and press Connect, or try Demo mode.');
  if (saved) connect();
})();
