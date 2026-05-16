// ── dashboard.js — Main dashboard with SSE real-time updates ─────────────────

let eventSource = null;

// ── Backend copy table ──────────────────────────────────────────────────────
// Keyed by (backend.id, errorCode). The C++ server emits structured status;
// this table owns every user-facing string. Adding a new error code on the
// server falls back to a generic message until a matching entry is added here.
const BACKEND_COPY = {
  vigem: {
    title: 'ViGEmBus Driver',
    pipelineLabel: 'ViGEm Submit',
    flowLabel: 'ViGEmBus',
    statusActive: 'Active (controllers plugged in)',
    statusIdle: 'Ready (idle — no controllers)',
    statusUnknown: 'Detected',
    errors: {
      DRIVER_MISSING: {
        title: 'ViGEmBus driver not detected',
        body: 'ViGEmBus is a kernel driver that lets Satellite create a virtual Xbox 360 controller on Windows. Without it, controller input cannot be forwarded.',
        steps: [
          { text: 'Download the latest ViGEmBus release', url: 'https://github.com/nefarius/ViGEmBus/releases' },
          { text: 'Run the installer (ViGEmBus_Setup_x64.msi)' },
          { text: 'Restart may be required — check Device Manager under "System devices" for "Nefarius Virtual Gamepad Emulation Bus"' },
          { text: 'Refresh this page; status should show "Detected"' },
        ],
      },
      BUS_OPEN_FAILED: {
        title: 'ViGEmBus driver detected but unresponsive',
        body: 'The driver is installed but Satellite could not open the bus. This usually means a version mismatch between the driver and the satellite build, or the driver service is stopped.',
        steps: [
          { text: 'Reinstall the latest ViGEmBus release', url: 'https://github.com/nefarius/ViGEmBus/releases' },
          { text: 'Or restart your machine and try again' },
        ],
      },
    },
  },
  uinput: {
    title: 'uinput Backend',
    pipelineLabel: 'uinput Inject',
    flowLabel: 'uinput',
    statusActive: 'Active (controllers plugged in)',
    statusIdle: 'Ready (idle — no controllers)',
    statusUnknown: '/dev/uinput accessible',
    errors: {
      DEVICE_MISSING: {
        title: '/dev/uinput not found',
        body: 'The uinput kernel module ships with every mainline Linux kernel but may not be loaded right now. Without it, Satellite cannot create virtual gamepads.',
        steps: [
          { text: 'Load the module now', command: 'sudo modprobe uinput' },
          { text: 'Make it persistent across reboots', command: "echo uinput | sudo tee /etc/modules-load.d/satellite.conf" },
          { text: 'Refresh this page once /dev/uinput exists' },
        ],
      },
      PERMISSION_DENIED: {
        title: 'No write access to /dev/uinput',
        body: '/dev/uinput exists but the user running Satellite cannot write to it. Add a udev rule and join the input group.',
        steps: [
          { text: 'Install a udev rule', command: "echo 'KERNEL==\"uinput\", GROUP=\"input\", MODE=\"0660\"' | sudo tee /etc/udev/rules.d/70-satellite-uinput.rules" },
          { text: 'Reload udev', command: 'sudo udevadm control --reload-rules && sudo udevadm trigger' },
          { text: 'Add yourself to the input group', command: 'sudo usermod -aG input "$USER"' },
          { text: 'Log out and back in for the group change to apply, then refresh this page' },
        ],
      },
    },
  },
};

// ── Per-controller motion (IMU) copy ────────────────────────────────────────
// Task 1.1 — gyro/accelerometer. Four states, derived from the controller's
// motionCapable / motionActive / motionSink flags in /api/connections. Like
// BACKEND_COPY above, this table owns every user-facing motion string.
//   na    — controller has no IMU (e.g. an Xbox pad): motion not available
//   ready — IMU present + advertised, but no motion packet received yet
//   on    — motion streaming AND reaching the OS-level virtual gamepad
//   dsu   — motion streaming, reaching DSU emulators only (not the virtual pad)
const MOTION_COPY = {
  na: {
    cls: 'motion-na',
    text: 'No motion',
    title: 'This controller has no gyroscope / accelerometer (IMU). Motion is not available for it — this is expected for Xbox-style pads.',
  },
  ready: {
    cls: 'motion-ready',
    text: 'Motion ready',
    title: 'The controller reports an IMU and the sender advertised motion support — waiting for the first motion packet.',
  },
  on: {
    cls: 'motion-on',
    text: 'Motion on',
    title: 'Gyro / accelerometer is streaming to the virtual gamepad AND to any Cemuhook DSU emulator (udp 26760).',
  },
  dsu: {
    cls: 'motion-dsu',
    text: 'Motion · DSU only',
    title: 'Gyro / accelerometer is streaming and reaching DSU emulators (udp 26760), but NOT the OS-level virtual gamepad. The virtual device has no IMU surface — an Xbox-typed device, a ViGEmBus older than 1.22, or the macOS backend.',
  },
};

// Resolve a controller's motion state id from the /api/connections flags.
function motionStateId(ctrl) {
  if (!ctrl.motionCapable) return 'na';
  if (!ctrl.motionActive) return 'ready';
  return ctrl.motionSink ? 'on' : 'dsu';
}

// ── Per-controller battery chip ─────────────────────────────────────────────
// Task 1.2 — battery level reporting. Derived from `ctrl.battery` in
// /api/connections: either { level: 0..100 | null, status: "..." } or null.
// The sender forwards the controller's own battery, or — when the controller
// is wired/USB — the host machine's battery (laptop %, or 100% on a desktop).
// Returns { cls, text, title }, mirroring MOTION_COPY's shape.
function batteryChip(ctrl) {
  const b = ctrl.battery;
  if (!b) {
    return {
      cls: 'battery-na',
      text: 'No battery',
      title: 'This sender has not reported a battery level for this controller.',
    };
  }
  const lvl = (typeof b.level === 'number') ? b.level : null;
  const pct = (lvl === null) ? '—' : lvl + '%';

  let cls = 'battery-ok';
  if (b.status === 'charging') {
    cls = 'battery-charging';
  } else if (lvl !== null && lvl < 10) {
    cls = 'battery-crit';
  } else if (lvl !== null && lvl < 20) {
    cls = 'battery-low';
  }

  let text, title;
  switch (b.status) {
    case 'charging':
      text = 'Battery ' + pct;
      title = 'Battery ' + pct + ' — charging.';
      break;
    case 'full':
      text = 'Battery ' + (lvl === null ? 'full' : pct);
      title = 'Battery full.';
      break;
    case 'wired':
      text = (lvl === null) ? 'AC power' : 'Battery ' + pct;
      title = 'AC powered — a wired controller falls back to the host machine’s '
            + 'battery (100% on a desktop).';
      break;
    case 'discharging':
      text = 'Battery ' + pct;
      title = 'Battery ' + pct + ' — discharging.';
      break;
    default:
      text = 'Battery ' + pct;
      title = 'Battery level ' + pct + ' (charging state unknown).';
  }
  return { cls, text, title };
}

function initDashboard() {
  startSSE();
  loadDevices();
  checkBackendStatus();
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
      // Capture active-connection count for the restart-confirmation modal.
      window.__activeConnectionCount = (d.connections || []).length;
    } catch (err) { /* ignore */ }
  });

  eventSource.addEventListener('update', (e) => {
    try {
      const d = JSON.parse(e.data);
      if (typeof updatesHandleSSE === 'function') updatesHandleSSE(d);
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

  // Update backend status from SSE data if available
  if (d.backend) {
    updateBackendPanel(d.backend, d.backendAvailable);
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
        <button class="btn-icon btn-danger" onclick="disconnectConn('${esc(c.connectionId)}')" title="Disconnect"><img src="img/icons/close_x.svg" alt="Disconnect" class="emoji-icon"></button>
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
        const ok = ctrl.pluggedIn;
        const ctrlType = ctrl.controllerType || 'xbox';
        const ctrlLabel = ctrl.controllerTypeLabel || 'Xbox';
        const m = MOTION_COPY[motionStateId(ctrl)] || MOTION_COPY.na;
        const bat = batteryChip(ctrl);
        return `
        <div class="ctrl-item">
          <div class="ctrl-row">
            <img class="ctrl-type-icon" src="img/ctrl-${esc(ctrlType)}.svg" alt="${esc(ctrlLabel)}" title="${esc(ctrlLabel)}">
            <div class="ctrl-info">
              <span class="ctrl-name"><span class="ctrl-dot ${ok ? 'ok' : 'err'}"></span>Controller #${ctrl.controllerIndex} · ${esc(ctrlLabel)}</span>
              <span class="ctrl-meta">${esc(ctrl.deviceName)} · Serial ${ctrl.serialNo} · ${ok ? 'Plugged In' : 'Error'}</span>
            </div>
          </div>
          <div class="ctrl-chips">
            <span class="ctrl-battery ${bat.cls}" title="${esc(bat.title)}">${esc(bat.text)}</span>
            <span class="ctrl-motion ${m.cls}" title="${esc(m.title)}">${esc(m.text)}</span>
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
  checkBackendStatus();
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
        <button class="btn-icon btn-danger" onclick="removeDevice('${esc(d.id)}')" title="Remove"><img src="img/icons/close_x.svg" alt="Remove" class="emoji-icon"></button>
      </div>
    `).join('');
  } catch (e) { /* ignore */ }
}

async function removeDevice(id) {
  await apiPost('/api/devices/remove', { id });
  loadDevices();
}

// ── Backend status ──────────────────────────────────────────────────────────
// `backend` is { id, supported, available, errorCode } from /api/backend/status
// or the SSE status stream. `backendActive` (optional) is the runtime "bus is
// open with controllers plugged in" flag from the SessionService.
let backendGuideOpen = false;

function updateBackendPanel(backend, backendActive) {
  const section = document.getElementById('backend-section');
  if (!section) return;

  // macOS / unsupported: hide the panel entirely.
  if (!backend || !backend.supported) {
    section.style.display = 'none';
    return;
  }
  section.style.display = '';

  const copy = BACKEND_COPY[backend.id] || {};
  const titleEl  = document.getElementById('backend-title');
  const flowText = document.getElementById('flow-backend-text');
  if (titleEl)  titleEl.textContent  = copy.title || 'Backend';
  if (flowText) flowText.textContent = copy.flowLabel || backend.id;

  const dot     = document.getElementById('backend-dot');
  const label   = document.getElementById('backend-label');
  const toggle  = document.getElementById('backend-guide-toggle');
  const flowBe  = document.getElementById('flow-backend');
  const flowSys = document.getElementById('flow-system');
  const guide   = document.getElementById('backend-guide');

  if (backend.available) {
    dot.className   = 'backend-dot backend-ok';
    label.textContent = backendActive ? (copy.statusActive || 'Active')
                                      : (copy.statusIdle   || 'Ready');
    label.className = 'backend-label backend-ok-text';
    toggle.style.display = 'none';
    flowBe.className  = 'flow-step done';
    flowSys.className = 'flow-step ' + (backendActive ? 'done' : '');
    guide.style.display = 'none';
    backendGuideOpen = false;
  } else {
    dot.className   = 'backend-dot backend-err';
    const err = (copy.errors && copy.errors[backend.errorCode]) || null;
    label.textContent = err ? err.title : 'Backend unavailable';
    label.className = 'backend-label backend-err-text';
    toggle.style.display = '';
    flowBe.className  = 'flow-step fail';
    flowSys.className = 'flow-step fail';
    populateBackendGuide(err);
  }
}

function populateBackendGuide(err) {
  const titleEl = document.getElementById('backend-guide-title');
  const bodyEl  = document.getElementById('backend-guide-body');
  const stepsEl = document.getElementById('backend-guide-steps');
  if (!titleEl || !bodyEl || !stepsEl) return;

  if (!err) {
    titleEl.textContent = 'No remediation available';
    bodyEl.textContent  = 'The server reported a state we don’t have copy for yet.';
    stepsEl.innerHTML   = '';
    return;
  }
  titleEl.textContent = err.title;
  bodyEl.textContent  = err.body;
  stepsEl.innerHTML = (err.steps || []).map(s => {
    let inner = esc(s.text);
    if (s.command) inner += `<pre class="guide-cmd">${esc(s.command)}</pre>`;
    if (s.url)     inner += ` <a href="${esc(s.url)}" target="_blank" rel="noopener">${esc(s.url)}</a>`;
    return `<li>${inner}</li>`;
  }).join('');
}

async function checkBackendStatus() {
  try {
    const r = await fetch('/api/backend/status');
    if (!r.ok) return;
    const d = await r.json();
    updateBackendPanel(d, d.available);
  } catch (e) { /* ignore */ }
}

function toggleBackendGuide() {
  backendGuideOpen = !backendGuideOpen;
  const guide = document.getElementById('backend-guide');
  const btn   = document.getElementById('backend-guide-toggle');
  guide.style.display = backendGuideOpen ? 'block' : 'none';
  btn.innerHTML = backendGuideOpen
    ? 'Setup Guide <img src="img/icons/chevron_down.svg" alt="" class="emoji-icon">'
    : 'Setup Guide <img src="img/icons/chevron_right.svg" alt="" class="emoji-icon">';
}

// ── Logout ──────────────────────────────────────────────────────────────────
async function doLogout() {
  await apiPost('/api/auth/logout');
  stopSSE();
  navigate('/login');
}

