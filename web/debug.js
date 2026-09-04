let debugTimer = null;
let debugBackendTimer = null;
let prevSnap = null;
let prevTime = null;
const rxHistory = [];
const txHistory = [];
const MAX_HISTORY = 60;
let debugRowsBuilt = false;
let lastDebugBackends = null;

const DEBUG_POLL_MS = 500;
const DEBUG_BACKEND_POLL_MS = 10000;

function dashIfNull(v) {
  return (v === null || v === undefined) ? '—' : v;
}

function count(v) {
  return (typeof v === 'number') ? v.toLocaleString() : '—';
}

function countCls(v, cls) {
  if (typeof v !== 'number') return { text: '—', cls: '' };
  return { text: v.toLocaleString(), cls: v > 0 ? cls : 'debug-ok' };
}

function bytesText(v) {
  if (typeof v !== 'number') return '—';
  if (v < 1024) return v + ' B';
  if (v < 1024 * 1024) return (v / 1024).toFixed(1) + ' KB';
  if (v < 1024 * 1024 * 1024) return (v / (1024 * 1024)).toFixed(2) + ' MB';
  return (v / (1024 * 1024 * 1024)).toFixed(2) + ' GB';
}

function usText(v) {
  return (typeof v === 'number') ? v.toLocaleString() + ' µs' : '—';
}

function boolText(v) {
  if (typeof v !== 'boolean') return '—';
  return v ? t('debug.value.yes') : t('debug.value.no');
}

function boolRow(v, goodWhenTrue) {
  if (typeof v !== 'boolean') return { text: '—', cls: '' };
  const good = goodWhenTrue ? v : !v;
  return { text: boolText(v), cls: good ? 'debug-ok' : 'debug-warn' };
}

function loopCls(v) {
  if (typeof v !== 'number') return '';
  if (v > 1000) return 'debug-err';
  if (v > 500) return 'debug-warn';
  return 'debug-ok';
}

function sub(d, group) {
  return (d && typeof d[group] === 'object' && d[group] !== null) ? d[group] : {};
}

const DEBUG_GROUPS = [
  {
    title: 'debug.section.inbound',
    rows: [
      { id: 'rx-rate', k: 'debug.rx.rate', v: (d, r) => r.rxPps + ' pps' },
      { id: 'rx-total', k: 'debug.stats.packets-received', v: d => count(d.packets) },
      { id: 'rx-input', k: 'debug.rx.input', v: d => count(sub(d, 'rx').input) },
      { id: 'rx-heartbeat', k: 'debug.rx.heartbeat', v: d => count(sub(d, 'rx').heartbeat) },
      { id: 'rx-motion', k: 'debug.rx.motion', v: d => count(sub(d, 'rx').motion) },
      { id: 'rx-battery', k: 'debug.rx.battery', v: d => count(sub(d, 'rx').battery) },
      { id: 'rx-pointer', k: 'debug.rx.pointer', v: d => count(sub(d, 'rx').pointer) },
      { id: 'rx-mic', k: 'debug.rx.mic', v: d => count(sub(d, 'rx').micAudio) },
    ],
  },
  {
    title: 'debug.section.outbound',
    rows: [
      { id: 'tx-rate', k: 'debug.tx.rate', v: (d, r) => r.txPps + ' pps' },
      { id: 'tx-total', k: 'debug.tx.total', v: d => count(sub(d, 'tx').packets) },
      { id: 'tx-bytes', k: 'debug.tx.bytes', v: d => bytesText(sub(d, 'tx').bytes) },
      { id: 'tx-ack', k: 'debug.tx.heartbeat-ack', v: d => count(sub(d, 'tx').heartbeatAck) },
      { id: 'tx-rumble', k: 'debug.tx.rumble', v: d => count(sub(d, 'tx').rumble) },
      { id: 'tx-lightbar', k: 'debug.tx.lightbar', v: d => count(sub(d, 'tx').lightbar) },
      { id: 'tx-trigger', k: 'debug.tx.trigger-effects',
        v: d => count(sub(d, 'tx').triggerEffects) },
      { id: 'tx-leds', k: 'debug.tx.player-leds', v: d => count(sub(d, 'tx').playerLeds) },
      { id: 'tx-speaker', k: 'debug.tx.speaker', v: d => count(sub(d, 'tx').speakerAudio) },
      { id: 'tx-micled', k: 'debug.tx.mic-led', v: d => count(sub(d, 'tx').micLed) },
      { id: 'tx-close', k: 'debug.tx.session-close', v: d => count(sub(d, 'tx').sessionClose) },
    ],
  },
  {
    title: 'debug.section.latency',
    rows: [
      { id: 'last-loop', k: 'debug.stats.last-loop',
        v: d => ({ text: usText(d.lastLoopUs), cls: loopCls(d.lastLoopUs) }) },
      { id: 'peak-loop', k: 'debug.stats.max-loop',
        v: d => ({ text: usText(d.peakLoopUs), cls: loopCls(d.peakLoopUs) }) },
      { id: 'submit-ok', k: 'debug.stats.submitted-ok',
        v: d => ({ text: count(d.submitOk), cls: 'debug-ok' }) },
      { id: 'submit-fail', k: 'debug.stats.submit-errors',
        v: d => countCls(d.submitFail, 'debug-err') },
      { id: 'drop-rate', k: 'debug.stats.drop-rate', v: d => {
          const total = (d.submitOk || 0) + (d.submitFail || 0);
          const pct = total > 0 ? ((d.submitFail / total) * 100) : 0;
          return { text: pct.toFixed(2) + '%', cls: d.submitFail > 0 ? 'debug-err' : 'debug-ok' };
        } },
    ],
  },
  {
    title: 'debug.section.audio',
    rows: [
      { id: 'au-mic-ok', k: 'debug.audio.mic-accepted',
        v: d => count(sub(d, 'audio').micAccepted) },
      { id: 'au-mic-dec', k: 'debug.audio.mic-decoded',
        v: d => count(sub(d, 'audio').micDecoded) },
      { id: 'au-mic-fec', k: 'debug.audio.mic-fec',
        v: d => countCls(sub(d, 'audio').micFecRecovered, 'debug-warn') },
      { id: 'au-mic-conceal', k: 'debug.audio.mic-concealed',
        v: d => countCls(sub(d, 'audio').micConcealed, 'debug-warn') },
      { id: 'au-mic-late', k: 'debug.audio.mic-late',
        v: d => countCls(sub(d, 'audio').micLate, 'debug-warn') },
      { id: 'au-mic-drop', k: 'debug.audio.mic-dropped',
        v: d => countCls(sub(d, 'audio').micDropped, 'debug-err') },
      { id: 'au-spk-sent', k: 'debug.audio.speaker-sent',
        v: d => count(sub(d, 'audio').speakerSent) },
      { id: 'au-spk-silence', k: 'debug.audio.speaker-silence',
        v: d => count(sub(d, 'audio').speakerSilenceSuppressed) },
      { id: 'au-spk-encfail', k: 'debug.audio.speaker-encode-fail',
        v: d => countCls(sub(d, 'audio').speakerEncodeFailed, 'debug-err') },
      { id: 'au-spk-lock', k: 'debug.audio.speaker-contended',
        v: d => countCls(sub(d, 'audio').speakerLockContended, 'debug-warn') },
    ],
  },
  {
    title: 'debug.section.rejected',
    rows: [
      { id: 'rj-decrypt', k: 'debug.stats.decrypt-failures',
        v: d => countCls(d.decryptFail, 'debug-err') },
      { id: 'rj-replay', k: 'debug.stats.replay-drops',
        v: d => countCls(d.replayDrop, 'debug-warn') },
      { id: 'rj-malformed', k: 'debug.rx.malformed',
        v: d => countCls(sub(d, 'rx').malformed, 'debug-err') },
      { id: 'rj-unknown-type', k: 'debug.rx.unknown-type',
        v: d => countCls(sub(d, 'rx').unknownType, 'debug-warn') },
      { id: 'rj-runt', k: 'debug.rx.runt', v: d => countCls(sub(d, 'rx').runt, 'debug-warn') },
      { id: 'rj-token', k: 'debug.rx.unknown-token',
        v: d => countCls(sub(d, 'rx').unknownToken, 'debug-warn') },
      { id: 'rj-tx-route', k: 'debug.tx.unroutable',
        v: d => countCls(sub(d, 'tx').unroutable, 'debug-warn') },
      { id: 'rj-tx-encrypt', k: 'debug.tx.encrypt-failed',
        v: d => countCls(sub(d, 'tx').encryptFailed, 'debug-err') },
      { id: 'rj-tx-oversize', k: 'debug.tx.oversize',
        v: d => countCls(sub(d, 'tx').oversize, 'debug-err') },
      { id: 'rj-tx-send', k: 'debug.tx.send-failed',
        v: d => countCls(sub(d, 'tx').sendFailed, 'debug-err') },
      { id: 'rj-auth-pair', k: 'debug.auth.not-paired',
        v: d => countCls(sub(d, 'auth').notPaired, 'debug-warn') },
      { id: 'rj-auth-proof', k: 'debug.auth.bad-proof',
        v: d => countCls(sub(d, 'auth').badProof, 'debug-err') },
      { id: 'rj-reaped', k: 'debug.sessions-reaped',
        v: d => countCls(d.sessionsReaped, 'debug-warn') },
    ],
  },
  {
    title: 'debug.section.host',
    rows: [
      { id: 'h-sender', k: 'debug.stats.sender-ip',
        v: d => d.senderIP || t('debug.sender.none') },
      { id: 'h-udp', k: 'debug.stats.udp-port', v: d => dashIfNull(d.udpPort) },
      { id: 'h-http', k: 'debug.stats.http-port',
        v: d => dashIfNull(d.webPort || Number(location.port) || null) },
      { id: 'h-client-api', k: 'debug.host.client-api',
        v: d => boolRow(d.clientApiListening, true) },
      { id: 'h-mdns', k: 'debug.host.mdns', v: d => boolRow(d.mdnsResponderActive, true) },
      { id: 'h-conns', k: 'debug.host.connections', v: d => count(d.connections) },
      { id: 'h-ctrls', k: 'debug.host.controllers', v: d => {
          if (typeof d.controllers !== 'number') return '—';
          return d.controllers + ' / ' + (d.maxControllers || 16);
        } },
    ],
  },
];

function buildDebugRows() {
  const host = document.getElementById('debug-groups');
  if (!host) return;
  let html = '';
  for (const g of DEBUG_GROUPS) {
    html += '<div class="section"><h2 class="section-title">' + esc(t(g.title)) + '</h2>' +
            '<div class="debug-grid">';
    for (const row of g.rows) {
      html += '<div class="debug-stat">' +
              '<span class="debug-stat-label">' + esc(t(row.k)) + '</span>' +
              '<span class="debug-stat-value" id="d-' + row.id + '">—</span>' +
              '</div>';
    }
    html += '</div></div>';
  }
  host.innerHTML = html;
  debugRowsBuilt = true;
}

function applyDebugRows(d, rates) {
  for (const g of DEBUG_GROUPS) {
    for (const row of g.rows) {
      const el = document.getElementById('d-' + row.id);
      if (!el) continue;
      let out;
      try {
        out = row.v(d, rates);
      } catch (e) {
        out = '—';
      }
      if (out && typeof out === 'object') {
        el.textContent = out.text;
        el.className = 'debug-stat-value' + (out.cls ? ' ' + out.cls : '');
      } else {
        el.textContent = String(out);
        el.className = 'debug-stat-value';
      }
    }
  }
}

function initDebug() {
  prevSnap = null;
  prevTime = null;
  rxHistory.length = 0;
  txHistory.length = 0;
  const chart = document.getElementById('d-chart');
  if (chart) chart.textContent = '';
  if (!debugRowsBuilt) buildDebugRows();
  startDebugPolling();
}

function startDebugPolling() {
  stopDebugPolling();
  pollDebug();
  pollDebugBackends();
  debugTimer = setInterval(pollDebug, DEBUG_POLL_MS);
  debugBackendTimer = setInterval(pollDebugBackends, DEBUG_BACKEND_POLL_MS);
}

function stopDebugPolling() {
  if (debugTimer) { clearInterval(debugTimer); debugTimer = null; }
  if (debugBackendTimer) { clearInterval(debugBackendTimer); debugBackendTimer = null; }
}

function debugRates(d, now) {
  const r = { rxPps: 0, txPps: 0, submitPps: 0, eventPps: 0 };
  if (!prevSnap || !prevTime) return r;
  const dt = (now - prevTime) / 1000;
  if (dt <= 0) return r;
  const per = (a, b) => Math.max(0, Math.round(((a || 0) - (b || 0)) / dt));
  r.rxPps = per(d.packets, prevSnap.packets);
  r.submitPps = per(d.submitOk, prevSnap.submitOk);
  const tx = sub(d, 'tx');
  const ptx = sub(prevSnap, 'tx');
  r.txPps = per(tx.packets, ptx.packets);
  const events = o => (o.packets || 0) - (o.heartbeatAck || 0) - (o.sessionClose || 0);
  r.eventPps = per(events(tx), events(ptx));
  return r;
}

function backendStateFor(d) {
  const be = d.backend;
  if (be && be.supported && !be.available) return 'error';
  if (d.backendAvailable) return 'active';
  return 'idle';
}

function renderPipeline(d, rates) {
  const setText = (id, text) => {
    const el = document.getElementById(id);
    if (el) el.textContent = text;
  };
  setText('d-rx-pps', rates.rxPps + ' pps');
  setText('d-tx-pps', rates.txPps + ' pps');
  setText('d-submit-pps', rates.submitPps + ' pps');
  setText('d-event-pps', rates.eventPps + ' pps');
  setText('d-client-ip',
          (d.senderIP && d.senderIP !== 'none') ? d.senderIP : t('debug.sender.none'));
  setText('d-status', d.listening ? t('debug.status.active') : t('debug.status.stopped'));

  const beLabel = document.getElementById('pipe-backend-label');
  const beIcon = document.getElementById('pipe-backend-icon');
  const copy = (d.backend && typeof backendCopy === 'function') ? backendCopy(d.backend.id) : null;
  if (beLabel && copy && copy.pipelineLabel) beLabel.textContent = copy.pipelineLabel;
  if (beIcon && copy && copy.icon) beIcon.src = copy.icon;

  const beState = backendStateFor(d);
  setText('d-backend-state',
          beState === 'error' ? t('debug.status.unavailable')
                              : (beState === 'active' ? t('debug.status.active')
                                                      : t('debug.status.idle')));

  const cls = (id, name) => {
    const el = document.getElementById(id);
    if (el) el.className = name;
  };
  const flowing = d.listening && rates.rxPps > 0;
  const stage = d.listening ? (flowing ? ' pipe-active' : ' pipe-idle') : '';
  cls('pipe-client', 'pipe-stage' + stage);
  cls('pipe-satellite', 'pipe-stage' + stage);
  cls('pipe-backend', 'pipe-stage ' +
      (beState === 'error' ? 'pipe-error' : (beState === 'active' ? 'pipe-active' : 'pipe-idle')));
  cls('d-rx-arrow', 'pipe-dir' + (rates.rxPps > 0 ? ' pipe-flow' : ''));
  cls('d-tx-arrow', 'pipe-dir pipe-dir-rev' + (rates.txPps > 0 ? ' pipe-flow' : ''));
  cls('d-submit-arrow', 'pipe-dir' + (rates.submitPps > 0 ? ' pipe-flow' : ''));
  cls('d-event-arrow', 'pipe-dir pipe-dir-rev' + (rates.eventPps > 0 ? ' pipe-flow' : ''));
}

async function pollDebug() {
  let d;
  try {
    const r = await fetch('/api/debug');
    if (!r.ok) return;
    d = await r.json();
  } catch (e) {
    return;
  }
  const now = performance.now();
  const rates = debugRates(d, now);
  prevSnap = d;
  prevTime = now;

  renderPipeline(d, rates);
  applyDebugRows(d, rates);

  rxHistory.push(rates.rxPps);
  txHistory.push(rates.txPps);
  if (rxHistory.length > MAX_HISTORY) rxHistory.shift();
  if (txHistory.length > MAX_HISTORY) txHistory.shift();
  renderChart();
}

async function pollDebugBackends() {
  try {
    const r = await fetch('/api/backend/status');
    if (!r.ok) return;
    const d = await r.json();
    lastDebugBackends = Array.isArray(d.backends) ? d.backends : (d.id ? [d] : []);
    renderDebugBackends();
  } catch (e) { /* keep the last snapshot on screen */ }
}

function backendDisplayName(b) {
  const copy = (typeof backendCopy === 'function') ? backendCopy(b.id) : null;
  if (copy && copy.title) return copy.title;
  return b.displayName || b.id || '—';
}

function backendStatusChip(b) {
  const copy = (typeof backendCopy === 'function') ? backendCopy(b.id) : null;
  if (b.available) {
    return { text: (copy && copy.statusUnknown) || t('debug.status.active'), cls: 'debug-ok' };
  }
  const err = copy && copy.errors && copy.errors[b.errorCode];
  return {
    text: err ? err.title : (b.errorCode || t('debug.status.unavailable')),
    cls: 'debug-err',
  };
}

function renderDebugBackends() {
  const host = document.getElementById('debug-backends');
  if (!host) return;
  const list = lastDebugBackends;
  if (!Array.isArray(list) || list.length === 0) {
    host.innerHTML = '<p class="hint">' + esc(t('debug.backends.none')) + '</p>';
    return;
  }
  let html = '';
  for (const b of list) {
    if (!b) continue;
    const copy = (typeof backendCopy === 'function') ? backendCopy(b.id) : null;
    const icon = (copy && copy.icon) ? copy.icon : 'img/icons/gamepad_virtual.svg';
    const chip = backendStatusChip(b);
    const tags = [b.kernelMode ? t('debug.backends.kernel') : t('debug.backends.user')];
    if (b.audio) tags.push(t('debug.backends.audio'));
    if (b.lifecycle && b.lifecycle !== 'supported') tags.push(b.lifecycle);
    const meta = [b.vendor, tags.join(' · ')].filter(Boolean).join(' · ');
    const bundled = (b.bundledVersion && b.bundledVersion !== b.driverVersion)
      ? '<span class="debug-backend-bundled">' +
        esc(t('debug.backends.bundled', [b.bundledVersion])) + '</span>'
      : '';
    html += '<div class="debug-backend">' +
            '<img class="debug-backend-icon" src="' + esc(icon) + '" alt="">' +
            '<div class="debug-backend-body">' +
            '<span class="debug-backend-name">' + esc(backendDisplayName(b)) + '</span>' +
            '<span class="debug-backend-meta">' + esc(meta) + '</span>' +
            '</div>' +
            '<div class="debug-backend-right">' +
            '<span class="debug-stat-value ' + chip.cls + '">' + esc(chip.text) + '</span>' +
            '<span class="debug-backend-version">' + esc(b.driverVersion || '—') + bundled +
            '</span>' +
            '</div></div>';
  }
  host.innerHTML = html;
}

function chartColumns(rxMax, txMax) {
  const half = 34;
  let html = '';
  for (let i = 0; i < rxHistory.length; i++) {
    const rh = Math.max(1, Math.round((rxHistory[i] / rxMax) * half));
    const th = Math.max(1, Math.round((txHistory[i] / txMax) * half));
    html += '<div class="chart-col">' +
            '<div class="chart-half-up">' +
            '<div class="chart-seg chart-seg-rx" style="height:' + rh + 'px" title="' +
            rxHistory[i] + ' pps"></div></div>' +
            '<div class="chart-half-down">' +
            '<div class="chart-seg chart-seg-tx" style="height:' + th + 'px" title="' +
            txHistory[i] + ' pps"></div></div>' +
            '</div>';
  }
  return html;
}

function renderChart() {
  const chart = document.getElementById('d-chart');
  if (!chart || rxHistory.length === 0) return;
  const rxMax = Math.max(...rxHistory, 1);
  const txMax = Math.max(...txHistory, 1);
  chart.innerHTML =
    '<div class="chart-scale">' +
    '<span class="chart-scale-rx">' + esc(t('debug.chart.in')) + ' ' + rxMax + ' pps</span>' +
    '<span class="chart-scale-tx">' + esc(t('debug.chart.out')) + ' ' + txMax + ' pps</span>' +
    '</div>' +
    '<div class="chart-cols">' + chartColumns(rxMax, txMax) + '</div>';
}

const _debugOrigShowView = showView;
showView = function (id) {
  if (id !== 'view-debug') stopDebugPolling();
  _debugOrigShowView(id);
};
