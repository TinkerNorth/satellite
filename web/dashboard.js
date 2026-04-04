// ── dashboard.js — Main dashboard with SSE real-time updates ─────────────────

let eventSource = null;

function initDashboard() {
  startSSE();
  loadDevices();
  checkVigemStatus();
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
    stopSSE();
    // Check if server is truly unreachable vs just a transient SSE glitch
    fetch('/api/auth/status', { signal: AbortSignal.timeout(3000) })
      .then(r => {
        if (r.ok) {
          // Server is still up, just SSE dropped — reconnect
          setTimeout(startSSE, 2000);
        } else if (r.status === 401) {
          // Session expired
          navigate('/login');
        } else {
          showOffline();
        }
      })
      .catch(() => {
        // Server unreachable
        showOffline();
      });
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

  // Update ViGEm status from SSE data if available
  if (d.vigemInstalled !== undefined) {
    updateVigemIndicator(d.vigemInstalled, d.vigemAvailable);
  }
}

function updateConnections(d) {
  const connEl = document.getElementById('connection-list');
  const ctrlEl = document.getElementById('controller-list');
  const countEl = document.getElementById('controller-count');

  if (!d.connections || d.connections.length === 0) {
    if (connEl) connEl.innerHTML = '<p class="hint">No active connections</p>';
    if (ctrlEl) ctrlEl.innerHTML = '<p class="hint">No active controllers</p>';
    if (countEl) countEl.textContent = '0 / ' + (d.maxControllers || 16);
    return;
  }

  // ── Connections list (network sessions) ──
  if (connEl) {
    connEl.innerHTML = d.connections.map(c => `
      <div class="device-item">
        <div class="device-info">
          <span class="device-name">${esc(c.deviceName)}</span>
          <span class="device-meta">${esc(c.senderIP)} · ${c.activeControllerCount || 0} controller${(c.activeControllerCount||0) === 1 ? '' : 's'}</span>
        </div>
        <button class="btn-icon btn-danger" onclick="disconnectConn('${esc(c.connectionId)}')" title="Disconnect">✕</button>
      </div>`).join('');
  }

  // ── Virtual controllers list (per-device ViGEm state) ──
  if (ctrlEl) {
    const allCtrls = [];
    d.connections.forEach(c => {
      c.controllers.forEach(ctrl => {
        allCtrls.push({ ...ctrl, deviceName: c.deviceName, connectionId: c.connectionId });
      });
    });
    if (allCtrls.length === 0) {
      ctrlEl.innerHTML = '<p class="hint">No active controllers</p>';
    } else {
      ctrlEl.innerHTML = allCtrls.map(ctrl => {
        const ok = ctrl.vigemPluggedIn;
        const ctrlType = ctrl.controllerType || 'xbox';
        const ctrlLabel = ctrl.controllerTypeLabel || 'Xbox';
        return `
        <div class="ctrl-item">
          <div class="ctrl-row">
            <img class="ctrl-type-icon" src="img/ctrl-${esc(ctrlType)}.svg" alt="${esc(ctrlLabel)}" title="${esc(ctrlLabel)}">
            <div class="ctrl-info">
              <span class="ctrl-name"><span class="ctrl-dot ${ok ? 'ok' : 'err'}"></span>Controller #${ctrl.controllerIndex} · ${esc(ctrlLabel)}</span>
              <span class="ctrl-meta">${esc(ctrl.deviceName)} · ViGEm Serial ${ctrl.vigemSerialNo} · ${ok ? 'Plugged In' : 'Error'}</span>
            </div>
          </div>
        </div>`;
      }).join('');
    }
  }

  if (countEl) countEl.textContent = d.totalControllers + ' / ' + d.maxControllers;
}

async function poll() {
  try {
    const r = await fetch('/api/status');
    if (r.status === 401) { stopSSE(); navigate('/login'); return; }
    const d = await r.json();
    updateStatus(d);
  } catch (e) {
    // Server unreachable during poll
    showOffline();
    return;
  }
  checkVigemStatus();
}

// ── Actions ─────────────────────────────────────────────────────────────────
async function toggle() {
  const r = await fetch('/api/status');
  const d = await r.json();
  await apiPost(d.listening ? '/api/stop' : '/api/start');
  setTimeout(poll, 300);
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

function updateVigemIndicator(installed, available) {
  const dot = document.getElementById('vigem-dot');
  const label = document.getElementById('vigem-label');
  const toggle = document.getElementById('vigem-guide-toggle');
  const flowVigem = document.getElementById('flow-vigem');
  const flowSystem = document.getElementById('flow-system');
  if (!dot || !label) return;

  if (installed && available) {
    dot.className = 'vigem-dot vigem-ok';
    label.textContent = 'Active (controllers plugged in)';
    label.className = 'vigem-label vigem-ok-text';
    toggle.style.display = 'none';
    flowVigem.className = 'flow-step done';
    flowSystem.className = 'flow-step done';
    document.getElementById('vigem-guide').style.display = 'none';
    vigemGuideOpen = false;
  } else if (installed) {
    dot.className = 'vigem-dot vigem-ok';
    label.textContent = 'Ready (idle — no controllers)';
    label.className = 'vigem-label vigem-ok-text';
    toggle.style.display = 'none';
    flowVigem.className = 'flow-step done';
    flowSystem.className = 'flow-step';
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
}

async function checkVigemStatus() {
  try {
    const r = await fetch('/api/vigem/status');
    if (!r.ok) return;
    const d = await r.json();
    updateVigemIndicator(d.installed, d.available);
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

