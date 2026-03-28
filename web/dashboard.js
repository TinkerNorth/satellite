// ── dashboard.js — Main dashboard with SSE real-time updates ─────────────────

let configDirty = false;
let savedConfig = { udpPort: 9876, autoStart: false };
let eventSource = null;

function checkConfigDirty() {
  const curPort = parseInt(document.getElementById('udpPort').value);
  const curAuto = document.getElementById('autoStart').checked;
  const dirty = curPort !== savedConfig.udpPort || curAuto !== savedConfig.autoStart;
  configDirty = dirty;
  document.getElementById('btnSave').disabled = !dirty;
  document.getElementById('btnUndo').disabled = !dirty;
}

function undoConfig() {
  document.getElementById('udpPort').value = savedConfig.udpPort;
  document.getElementById('autoStart').checked = savedConfig.autoStart;
  checkConfigDirty();
}

function initDashboard() {
  startSSE();
  loadDevices();
  checkVigemStatus();

  const udp = document.getElementById('udpPort');
  const auto_ = document.getElementById('autoStart');
  if (udp) udp.addEventListener('input', checkConfigDirty);
  if (auto_) auto_.addEventListener('change', checkConfigDirty);
}

// ── SSE (replaces polling) ──────────────────────────────────────────────────
function startSSE() {
  stopSSE();
  eventSource = new EventSource('/api/events');

  eventSource.addEventListener('status', (e) => {
    try {
      const d = JSON.parse(e.data);
      updateStatus(d);
    } catch (err) { /* ignore */ }
  });

  eventSource.addEventListener('connections', (e) => {
    try {
      const d = JSON.parse(e.data);
      updateConnections(d);
    } catch (err) { /* ignore */ }
  });

  eventSource.onerror = () => {
    // Reconnect after a short delay
    stopSSE();
    setTimeout(startSSE, 3000);
  };

  // Also do an initial fetch
  poll();
}

function stopSSE() {
  if (eventSource) { eventSource.close(); eventSource = null; }
}

function updateStatus(d) {
  document.getElementById('s-status').textContent = d.listening ? 'Listening' : 'Stopped';
  document.getElementById('s-packets').textContent = d.packets.toLocaleString();
  document.getElementById('s-sender').textContent = d.senderIP;
  document.getElementById('s-port').textContent = d.udpPort;
  document.getElementById('s-http-port').textContent = d.webPort || location.port || '—';

  const dot = document.getElementById('dot');
  dot.className = 'dot ' + (d.listening ? 'on' : 'off');

  const btn = document.getElementById('btnToggle');
  btn.textContent = d.listening ? 'Stop' : 'Start';
  btn.className = 'btn ' + (d.listening ? 'btn-stop' : 'btn-start');

  if (!configDirty) {
    savedConfig.udpPort = d.udpPort;
    savedConfig.autoStart = d.autoStart;
    document.getElementById('udpPort').value = d.udpPort;
    document.getElementById('autoStart').checked = d.autoStart;
  }
}

function updateConnections(d) {
  const el = document.getElementById('connection-list');
  if (!el) return;

  if (!d.connections || d.connections.length === 0) {
    el.innerHTML = '<p class="hint">No active connections</p>';
    const countEl = document.getElementById('controller-count');
    if (countEl) countEl.textContent = '0 / ' + (d.maxControllers || 16);
    return;
  }

  el.innerHTML = d.connections.map(c => {
    const ctrls = c.controllers.map(ctrl =>
      `<span class="ctrl-badge" title="Serial #${ctrl.vigemSerialNo}">🎮 #${ctrl.controllerIndex}</span>`
    ).join('');
    return `
      <div class="connection-item">
        <div class="connection-info">
          <span class="connection-name">${esc(c.deviceName)}</span>
          <span class="connection-meta">${esc(c.senderIP)} · ${ctrls || 'No controllers'}</span>
        </div>
        <button class="btn-icon btn-danger" onclick="disconnectConn('${esc(c.connectionId)}')" title="Disconnect">✕</button>
      </div>`;
  }).join('');

  const countEl = document.getElementById('controller-count');
  if (countEl) countEl.textContent = d.totalControllers + ' / ' + d.maxControllers;
}

async function poll() {
  try {
    const r = await fetch('/api/status');
    if (r.status === 401) { stopSSE(); navigate('/login'); return; }
    const d = await r.json();
    updateStatus(d);
  } catch (e) { /* ignore */ }
  checkVigemStatus();
}

// ── Actions ─────────────────────────────────────────────────────────────────
async function toggle() {
  const r = await fetch('/api/status');
  const d = await r.json();
  await apiPost(d.listening ? '/api/stop' : '/api/start');
  setTimeout(poll, 300);
}

async function saveConfig() {
  await apiPost('/api/config', {
    udpPort: parseInt(document.getElementById('udpPort').value),
    autoStart: document.getElementById('autoStart').checked
  });
  savedConfig.udpPort = parseInt(document.getElementById('udpPort').value);
  savedConfig.autoStart = document.getElementById('autoStart').checked;
  checkConfigDirty();
  poll();
}

// ── Connections ─────────────────────────────────────────────────────────────
async function disconnectConn(connId) {
  await fetch('/api/connections/' + connId, { method: 'DELETE' });
}

// ── PIN ─────────────────────────────────────────────────────────────────────
async function genPin() {
  const { ok, data } = await apiPost('/api/pin/generate');
  document.getElementById('pin-display').textContent = (ok && data.pin) ? data.pin : '—';
}

// ── Devices ─────────────────────────────────────────────────────────────────
async function loadDevices() {
  try {
    const r = await fetch('/api/devices');
    if (!r.ok) return;
    const devs = await r.json();
    const el = document.getElementById('device-list');
    if (devs.length === 0) {
      el.innerHTML = '<p class="hint">No paired devices</p>';
      return;
    }
    el.innerHTML = devs.map(d => `
      <div class="device-item">
        <div class="device-info">
          <span class="device-name">${esc(d.name)}</span>
          <span class="device-meta">${esc(d.lastIP)} · ${esc(d.pairedAt)}</span>
        </div>
        <button class="btn-icon btn-danger" onclick="removeDevice('${esc(d.id)}')" title="Remove">✕</button>
      </div>
    `).join('');
  } catch (e) { /* ignore */ }
}

async function removeDevice(id) {
  await apiPost('/api/devices/remove', { id });
  loadDevices();
}

// ── ViGEm Status ────────────────────────────────────────────────────────────
let vigemGuideOpen = false;

async function checkVigemStatus() {
  try {
    const r = await fetch('/api/vigem/status');
    if (!r.ok) return;
    const d = await r.json();
    const dot = document.getElementById('vigem-dot');
    const label = document.getElementById('vigem-label');
    const toggle = document.getElementById('vigem-guide-toggle');
    const flowVigem = document.getElementById('flow-vigem');
    const flowSystem = document.getElementById('flow-system');

    if (d.installed) {
      dot.className = 'vigem-dot vigem-ok';
      label.textContent = 'Detected';
      label.className = 'vigem-label vigem-ok-text';
      toggle.style.display = 'none';
      flowVigem.className = 'flow-step done';
      flowSystem.className = 'flow-step done';
      document.getElementById('vigem-guide').style.display = 'none';
      vigemGuideOpen = false;
    } else {
      dot.className = 'vigem-dot vigem-err';
      label.textContent = 'Not Detected';
      label.className = 'vigem-label vigem-err-text';
      toggle.style.display = '';
      flowVigem.className = 'flow-step fail';
      flowSystem.className = 'flow-step fail';
    }
  } catch (e) { /* ignore */ }
}

function toggleVigemGuide() {
  vigemGuideOpen = !vigemGuideOpen;
  const guide = document.getElementById('vigem-guide');
  const btn = document.getElementById('vigem-guide-toggle');
  guide.style.display = vigemGuideOpen ? 'block' : 'none';
  btn.textContent = vigemGuideOpen ? 'Setup Guide ▾' : 'Setup Guide ▸';
}

// ── Logout ──────────────────────────────────────────────────────────────────
async function doLogout() {
  await apiPost('/api/auth/logout');
  stopSSE();
  navigate('/login');
}

