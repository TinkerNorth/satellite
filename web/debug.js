// ── debug.js — Real-time debug telemetry page ──────────────────────────────

let debugTimer = null;
let prevSnap = null;
let prevTime = null;
const rateHistory = [];
const MAX_HISTORY = 60;

// Pull theme tokens from CSS custom properties so chart-bar colors stay in
// sync with style.css. See web/style.css :root and DESIGN.md.
function themeColor(name) {
  return getComputedStyle(document.documentElement).getPropertyValue(name).trim();
}

function initDebug() {
  prevSnap = null;
  prevTime = null;
  rateHistory.length = 0;
  document.getElementById('d-chart').textContent = '';
  startDebugPolling();
}

function startDebugPolling() {
  stopDebugPolling();
  pollDebug();
  debugTimer = setInterval(pollDebug, 500);
}

function stopDebugPolling() {
  if (debugTimer) { clearInterval(debugTimer); debugTimer = null; }
}

async function pollDebug() {
  try {
    const r = await fetch('/api/debug');
    if (r.status === 401) { stopDebugPolling(); navigate('/login'); return; }
    const d = await r.json();
    const now = performance.now();

    // ── Calculate rates from deltas ──
    let pps = 0, submitRate = 0;
    if (prevSnap && prevTime) {
      const dt = (now - prevTime) / 1000; // seconds
      if (dt > 0) {
        pps = Math.round((d.packets - prevSnap.packets) / dt);
        submitRate = Math.round((d.submitOk - prevSnap.submitOk) / dt);
      }
    }
    prevSnap = d;
    prevTime = now;

    // ── Update pipeline ──
    document.getElementById('d-pps').textContent = pps + ' pps';
    document.getElementById('d-submit-rate').textContent = submitRate + ' pps';
    document.getElementById('d-status').textContent = d.listening ? 'Active' : 'Stopped';

    // Pipeline coloring (reflects backend availability)
    const udp     = document.getElementById('pipe-udp');
    const backend = document.getElementById('pipe-backend');
    const sys     = document.getElementById('pipe-system');
    const a1      = document.getElementById('pipe-arrow-1');
    const a2      = document.getElementById('pipe-arrow-2');
    const backendUp = d.backendAvailable;

    // Driver-neutral pipeline label sourced from BACKEND_COPY when present.
    const beLabelEl = document.getElementById('pipe-backend-label');
    if (beLabelEl && d.backend && typeof BACKEND_COPY === 'object') {
      const copy = BACKEND_COPY[d.backend.id];
      if (copy && copy.pipelineLabel) beLabelEl.textContent = copy.pipelineLabel;
    }

    if (d.listening && pps > 0) {
      udp.className     = 'pipe-stage pipe-active';
      backend.className = 'pipe-stage ' + (backendUp ? 'pipe-active' : 'pipe-error');
      sys.className     = 'pipe-stage ' + (backendUp ? 'pipe-active' : 'pipe-error');
      a1.className      = 'pipe-arrow pipe-flow';
      a2.className      = 'pipe-arrow ' + (backendUp ? 'pipe-flow' : '');
    } else if (d.listening) {
      udp.className     = 'pipe-stage pipe-idle';
      backend.className = 'pipe-stage ' + (backendUp ? 'pipe-idle' : 'pipe-error');
      sys.className     = 'pipe-stage ' + (backendUp ? 'pipe-idle' : 'pipe-error');
      a1.className      = 'pipe-arrow';
      a2.className      = 'pipe-arrow';
    } else {
      udp.className     = 'pipe-stage';
      backend.className = 'pipe-stage' + (backendUp === false ? ' pipe-error' : '');
      sys.className     = 'pipe-stage';
      a1.className      = 'pipe-arrow';
      a2.className      = 'pipe-arrow';
    }

    // ── Update stats ──
    document.getElementById('d-packets').textContent = d.packets.toLocaleString();
    document.getElementById('d-submit-ok').textContent = d.submitOk.toLocaleString();
    document.getElementById('d-submit-fail').textContent = d.submitFail.toLocaleString();

    const total = d.submitOk + d.submitFail;
    const dropPct = total > 0 ? ((d.submitFail / total) * 100).toFixed(2) : '0.00';
    document.getElementById('d-drop-rate').textContent = dropPct + '%';
    document.getElementById('d-drop-rate').className =
      'debug-stat-value' + (d.submitFail > 0 ? ' debug-err' : ' debug-ok');

    document.getElementById('d-last-loop').textContent = d.lastLoopUs + ' µs';
    document.getElementById('d-max-loop').textContent = d.maxLoopUs + ' µs';
    document.getElementById('d-sender').textContent = d.senderIP;
    document.getElementById('d-port').textContent = d.udpPort;

    // ── Crypto stats ──
    const dfEl = document.getElementById('d-decrypt-fail');
    if (dfEl) dfEl.textContent = (d.decryptFail || 0).toLocaleString();
    const rdEl = document.getElementById('d-replay-drop');
    if (rdEl) rdEl.textContent = (d.replayDrop || 0).toLocaleString();

    // Color decrypt failures
    if (dfEl) dfEl.className = 'debug-stat-value' + ((d.decryptFail || 0) > 0 ? ' debug-err' : ' debug-ok');
    if (rdEl) rdEl.className = 'debug-stat-value' + ((d.replayDrop || 0) > 0 ? ' debug-warn' : ' debug-ok');

    // ── Backend status (single row, hidden on macOS / unsupported) ──
    const beRow   = document.getElementById('d-backend-row');
    const beLabel = document.getElementById('d-backend-label');
    const beVal   = document.getElementById('d-backend-value');
    if (beRow && beLabel && beVal && d.backend) {
      if (!d.backend.supported) {
        beRow.style.display = 'none';
      } else {
        beRow.style.display = '';
        const copy  = (typeof BACKEND_COPY === 'object' && BACKEND_COPY[d.backend.id]) || null;
        const title = (copy && copy.title) || 'Backend';
        beLabel.textContent = title;
        if (d.backend.available) {
          beVal.textContent = d.backendAvailable ? 'Active' : 'Idle';
          beVal.className   = 'debug-stat-value debug-ok';
        } else {
          const errCopy = copy && copy.errors && copy.errors[d.backend.errorCode];
          beVal.textContent = errCopy ? errCopy.title : (d.backend.errorCode || 'Unavailable');
          beVal.className   = 'debug-stat-value debug-err';
        }
      }
    }

    // Color the loop time
    const loopEl = document.getElementById('d-last-loop');
    if (d.lastLoopUs > 1000) loopEl.className = 'debug-stat-value debug-err';
    else if (d.lastLoopUs > 500) loopEl.className = 'debug-stat-value debug-warn';
    else loopEl.className = 'debug-stat-value debug-ok';

    // ── Rate history chart ──
    rateHistory.push(pps);
    if (rateHistory.length > MAX_HISTORY) rateHistory.shift();
    renderChart();
  } catch (e) { /* ignore */ }
}

function renderChart() {
  const chart = document.getElementById('d-chart');
  if (!chart || rateHistory.length === 0) return;
  const max = Math.max(...rateHistory, 1);
  const barH = 60; // max bar height in px
  const bars = rateHistory.map(v => {
    const h = Math.max(1, Math.round((v / max) * barH));
    const pct = v / max;
    const color = pct > 0.7 ? themeColor('--success')
                : pct > 0.3 ? themeColor('--primary')
                            : themeColor('--error');
    return `<div class="chart-bar" style="height:${h}px;background:${color}" title="${v} pps"></div>`;
  }).join('');
  chart.innerHTML = `<div class="chart-bars">${bars}</div><div class="chart-max">${max} pps</div>`;
}

