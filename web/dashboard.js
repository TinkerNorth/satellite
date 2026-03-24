// ── dashboard.js — Main dashboard with status, config, PIN, devices ─────────

let configDirty = false;
let pollTimer = null;

function setConfigDirty(dirty) {
  configDirty = dirty;
  const btn = document.getElementById('btnSave');
  if (btn) btn.disabled = !dirty;
}

function initDashboard() {
  startPolling();
  loadDevices();

  const udp = document.getElementById('udpPort');
  const auto_ = document.getElementById('autoStart');
  if (udp) udp.addEventListener('input', () => { setConfigDirty(true); });
  if (auto_) auto_.addEventListener('change', () => { setConfigDirty(true); });
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
      document.getElementById('udpPort').value = d.udpPort;
      document.getElementById('autoStart').checked = d.autoStart;
    }
  } catch (e) { /* ignore */ }
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
  setConfigDirty(false);
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

// ── Logout ──────────────────────────────────────────────────────────────────
async function doLogout() {
  await apiPost('/api/auth/logout');
  stopPolling();
  navigate('/login');
}

