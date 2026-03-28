// ── debug.js — Real-time debug telemetry page ──────────────────────────────

let debugTimer = null;
let prevSnap = null;
let prevTime = null;
const rateHistory = [];
const MAX_HISTORY = 60;

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

    // Pipeline coloring
    const udp = document.getElementById('pipe-udp');
    const vigem = document.getElementById('pipe-vigem');
    const sys = document.getElementById('pipe-system');
    const a1 = document.getElementById('pipe-arrow-1');
    const a2 = document.getElementById('pipe-arrow-2');

    if (d.listening && pps > 0) {
      udp.className = 'pipe-stage pipe-active';
      vigem.className = 'pipe-stage pipe-active';
      sys.className = 'pipe-stage pipe-active';
      a1.className = 'pipe-arrow pipe-flow';
      a2.className = 'pipe-arrow pipe-flow';
    } else if (d.listening) {
      udp.className = 'pipe-stage pipe-idle';
      vigem.className = 'pipe-stage pipe-idle';
      sys.className = 'pipe-stage pipe-idle';
      a1.className = 'pipe-arrow';
      a2.className = 'pipe-arrow';
    } else {
      udp.className = 'pipe-stage';
      vigem.className = 'pipe-stage';
      sys.className = 'pipe-stage';
      a1.className = 'pipe-arrow';
      a2.className = 'pipe-arrow';
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
    const color = pct > 0.7 ? '#22C55E' : pct > 0.3 ? '#FFC107' : '#E74C3C';
    return `<div class="chart-bar" style="height:${h}px;background:${color}" title="${v} pps"></div>`;
  }).join('');
  chart.innerHTML = `<div class="chart-bars">${bars}</div><div class="chart-max">${max} pps</div>`;
}

