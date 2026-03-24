// ── dashboard.js — Main dashboard with status, config, PIN, devices ─────────

let configDirty = false;
let pollTimer = null;
let savedConfig = { udpPort: 9876, autoStart: false };

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
  startPolling();
  loadDevices();
  checkVigemStatus();

  const udp = document.getElementById('udpPort');
  const auto_ = document.getElementById('autoStart');
  if (udp) udp.addEventListener('input', checkConfigDirty);
  if (auto_) auto_.addEventListener('change', checkConfigDirty);
}

// ── Polling ─────────────────────────────────────────────────────────────────
function startPolling() {
  poll();
  if (pollTimer) clearInterval(pollTimer);
  pollTimer = setInterval(poll, 1000);
}

function stopPolling() {
  if (pollTimer) { clearInterval(pollTimer); pollTimer = null; }
}

async function poll() {
  try {
    const r = await fetch('/api/status');
    if (r.status === 401) { stopPolling(); navigate('/login'); return; }
    const d = await r.json();

    document.getElementById('s-status').textContent = d.listening ? 'Listening' : 'Stopped';
    document.getElementById('s-packets').textContent = d.packets.toLocaleString();
    document.getElementById('s-sender').textContent = d.senderIP;
    document.getElementById('s-port').textContent = d.udpPort;

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
      // Auto-close guide if it was open
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
  stopPolling();
  navigate('/login');
}

