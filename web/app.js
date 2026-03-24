// ── State ────────────────────────────────────────────────────────────────────
let configDirty = false;
let pollTimer = null;

// ── View switching ──────────────────────────────────────────────────────────
function showView(id) {
  ['view-setup', 'view-login', 'view-dashboard'].forEach(v =>
    document.getElementById(v).style.display = v === id ? 'block' : 'none');
}

// ── Auth check on load ──────────────────────────────────────────────────────
async function checkAuth() {
  try {
    const r = await fetch('/api/auth/status');
    const d = await r.json();
    if (!d.configured) {
      showView('view-setup');
    } else if (!d.authenticated) {
      showView('view-login');
    } else {
      showView('view-dashboard');
      startPolling();
      loadDevices();
    }
  } catch (e) {
    showView('view-login');
  }
}

// ── Setup ───────────────────────────────────────────────────────────────────
async function doSetup() {
  const user = document.getElementById('setup-user').value.trim();
  const pass = document.getElementById('setup-pass').value;
  const pass2 = document.getElementById('setup-pass2').value;
  const err = document.getElementById('setup-error');

  if (!user) { err.textContent = 'Username is required'; return; }
  if (pass.length < 4) { err.textContent = 'Password must be at least 4 characters'; return; }
  if (pass !== pass2) { err.textContent = 'Passwords do not match'; return; }

  const r = await fetch('/api/auth/setup', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ username: user, password: pass })
  });
  const d = await r.json();
  if (r.ok) {
    showView('view-dashboard');
    startPolling();
    loadDevices();
  } else {
    err.textContent = d.error || 'Setup failed';
  }
}

// ── Login ───────────────────────────────────────────────────────────────────
async function doLogin() {
  const user = document.getElementById('login-user').value.trim();
  const pass = document.getElementById('login-pass').value;
  const err = document.getElementById('login-error');

  const r = await fetch('/api/auth/login', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ username: user, password: pass })
  });
  const d = await r.json();
  if (r.ok) {
    err.textContent = '';
    showView('view-dashboard');
    startPolling();
    loadDevices();
  } else {
    err.textContent = d.error || 'Login failed';
  }
}

// ── Logout ──────────────────────────────────────────────────────────────────
async function doLogout() {
  await fetch('/api/auth/logout', { method: 'POST' });
  stopPolling();
  showView('view-login');
}

// ── Dashboard polling ───────────────────────────────────────────────────────
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
    if (r.status === 401) { stopPolling(); showView('view-login'); return; }
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

async function toggle() {
  const r = await fetch('/api/status');
  const d = await r.json();
  await fetch(d.listening ? '/api/stop' : '/api/start', { method: 'POST' });
  setTimeout(poll, 300);
}

async function saveConfig() {
  const body = JSON.stringify({
    udpPort: parseInt(document.getElementById('udpPort').value),
    autoStart: document.getElementById('autoStart').checked
  });
  await fetch('/api/config', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body
  });
  configDirty = false;
  poll();
}

// ── PIN ─────────────────────────────────────────────────────────────────────
async function genPin() {
  const r = await fetch('/api/pin/generate', { method: 'POST' });
  const d = await r.json();
  document.getElementById('pin-display').textContent = d.pin || '—';
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
  await fetch('/api/devices/remove', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ id })
  });
  loadDevices();
}

function esc(s) {
  const d = document.createElement('div');
  d.textContent = s;
  return d.innerHTML;
}

// ── Init ────────────────────────────────────────────────────────────────────
document.addEventListener('DOMContentLoaded', () => {
  const udp = document.getElementById('udpPort');
  const auto_ = document.getElementById('autoStart');
  if (udp) udp.addEventListener('input', () => { configDirty = true; });
  if (auto_) auto_.addEventListener('change', () => { configDirty = true; });

  // Enter key on login/setup forms
  document.getElementById('login-pass').addEventListener('keydown', e => { if (e.key === 'Enter') doLogin(); });
  document.getElementById('setup-pass2').addEventListener('keydown', e => { if (e.key === 'Enter') doSetup(); });

  checkAuth();
});

