// ── dashboard.js — Main dashboard with SSE real-time updates ─────────────────

let eventSource = null;

// ── In-button loader helpers ───────────────────────────────────────────────
// Each non-atomic action on the dashboard (Generate PIN, Disconnect, Remove,
// Touchpad mode) flips its trigger into a "working" state while the fetch is
// in flight: button disabled, content replaced with spinner + label. The
// caller saves the original innerHTML, runs the request, then restores it.
// Kept inline so the two-stage pattern stays readable next to the call sites.

// Replace a button's contents with `<spinner> <label>` and disable it.
// Returns a restorer; call it (with no args) once the request settles to
// put the original markup back.
function setButtonLoading(btn, label) {
  if (!btn) return function () {};
  const prevHTML     = btn.innerHTML;
  const prevDisabled = btn.disabled;
  const prevAria     = btn.getAttribute('aria-busy');
  btn.disabled = true;
  btn.setAttribute('aria-busy', 'true');
  // Sizing: 12px for icon-only buttons (fits inside the 32×32 .btn-icon),
  // 14px for text buttons so the spinner reads alongside the Rajdhani label.
  const size = btn.classList.contains('btn-icon') ? 12 : 14;
  if (label) {
    btn.innerHTML = '<span class="btn-with-loader">' + spinnerSVG(size) + '<span>' + esc(label) + '</span></span>';
  } else {
    // Icon button — just swap the glyph for the spinner.
    btn.innerHTML = '<span class="btn-with-loader">' + spinnerSVG(size) + '</span>';
  }
  return function restore() {
    btn.innerHTML = prevHTML;
    btn.disabled  = prevDisabled;
    if (prevAria === null) btn.removeAttribute('aria-busy');
    else                   btn.setAttribute('aria-busy', prevAria);
  };
}

// ── Connection-state nomenclature (mirrors core/types.h) ───────────────────
// The server now stamps a lowercase `state` string onto each row in
// /api/connections, /api/devices, and per-controller in /api/connections, and
// pushes a `pin` SSE event. These tables map those wire strings onto the
// user-facing chip text. Keeping all copy in one place means swapping a chip
// label is a one-line change here, not a grep across the dashboard.

// DeviceLinkState → row chip (paired-devices section).
// "linking" is enumerated server-side but is currently never surfaced —
// /api/connections handshake is synchronous, so a device is either Paired
// (no live conn) or Active (live). The label is here for forward-compat.
const DEVICE_LINK_STATE_LABEL = {
  paired:        'Paired',
  linking:       'Connecting…',
  active:        'Online',
  notResponding: 'Not responding',
};

// ControllerState → per-controller tag (virtual-controllers section).
// Today the server only ever stamps "live" or "detached"; the transient
// states (registering, allocating) and the "failed" case are enumerated for
// when the SessionService starts threading them through.
const CONTROLLER_STATE_LABEL = {
  source:       'Detected',
  registering:  'Mounting…',
  allocating:   'Mounting…',
  live:         'Mounted',
  quiet:        'Mounted',
  detached:     'Unmounting…',
  failed:       'Blocked',
};

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
//   na     — controller has no IMU (e.g. an Xbox pad): motion not available
//   ready  — IMU present + advertised, but no motion packet received yet
//   on     — motion streaming AND reaching the OS-level virtual gamepad
//   nosink — motion streaming, but the virtual device exposes no IMU surface
//            to deliver it to (Xbox-typed device, old ViGEmBus, or macOS)
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
    title: 'Gyro / accelerometer is streaming to the OS-level virtual gamepad.',
  },
  nosink: {
    cls: 'motion-nosink',
    text: 'Motion not delivered',
    title: 'Gyro / accelerometer is being captured from the controller, but it is '
         + 'not delivered anywhere — this controller’s virtual device has no IMU '
         + 'surface to receive it (an Xbox-typed device, a ViGEmBus older than '
         + '1.22, or the macOS backend).',
  },
};

// Resolve a controller's motion state id from the /api/connections flags.
function motionStateId(ctrl) {
  if (!ctrl.motionCapable) return 'na';
  if (!ctrl.motionActive) return 'ready';
  return ctrl.motionSink ? 'on' : 'nosink';
}

// Map a battery (status, level) to its glyph in img/icons/. Status drives the
// charging icons; otherwise the level picks a rung of the charge ladder. This
// mirrors the percent → file table in the battery asset USAGE.md.
function batteryIconFile(b, lvl) {
  if (b.status === 'charging') return 'battery-charging-animated.svg';
  if (b.status === 'wired')    return 'battery-charging.svg';
  if (b.status === 'full')     return 'battery-full.svg';
  if (lvl === null)            return 'battery.svg';
  if (lvl <= 0)                return 'battery-empty.svg';
  if (lvl >= 90)               return 'battery-full.svg';
  if (lvl >= 60)               return 'battery-high.svg';
  if (lvl >= 35)               return 'battery-mid.svg';
  if (lvl >= 15)               return 'battery-low.svg';
  return 'battery-critical.svg';
}

// ── Per-controller battery chip ─────────────────────────────────────────────
// Task 1.2 — battery level reporting. Derived from `ctrl.battery` in
// /api/connections: either { level: 0..100 | null, status: "..." } or null.
// The sender forwards the controller's own battery, or — when the controller
// is wired/USB — the host machine's battery (laptop %, or 100% on a desktop).
// Returns { cls, text, title, icon }, mirroring MOTION_COPY's shape.
function batteryChip(ctrl) {
  const b = ctrl.battery;
  if (!b) {
    return {
      cls: 'battery-na',
      text: 'No battery',
      title: 'This sender has not reported a battery level for this controller.',
      icon: 'battery.svg',
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
  return { cls, text, title, icon: batteryIconFile(b, lvl) };
}

// ── Per-device touchpad routing (Task 1.3) ──────────────────────────────────
// The paired-device touchpad-mode selector. Mirrors TOUCHPAD_MODE_* on the
// server; the chosen mode is persisted in config and hot-applied to any live
// connection (no re-pairing). Pad → virtual DualShock 4 touchpad surface;
// Mouse → relative mouse pointer on this host; Off → ignore.
const TOUCHPAD_MODES = [
  { id: 'ds4',   label: 'Pad',
    title: 'Forward the touchpad into the virtual DualShock 4 controller. PlayStation-type controllers only — an Xbox virtual pad has no touchpad surface.' },
  { id: 'mouse', label: 'Mouse',
    title: 'Use the touchpad as a relative mouse on this machine — finger 0 moves the cursor, the clicky pad is the left mouse button.' },
  { id: 'off',   label: 'Off',
    title: 'Ignore touchpad input from this device.' },
];

// Per-controller touchpad chip — derived from the controller's `touchpadActive`
// flag, its `controllerType`, and the owning connection's `touchpadMode`.
// Mirrors MOTION_COPY's { cls, text, title } shape.
function touchpadChip(ctrl) {
  const mode = ctrl.touchpadMode || 'ds4';
  if (mode === 'off') {
    return { cls: 'touchpad-off', text: 'Touchpad off',
             title: 'Touchpad routing is turned off for this device — change it under Paired Devices.' };
  }
  // "Pad" mode forwards into the virtual DualShock 4 touchpad surface — but
  // that surface only exists on a PlayStation-typed virtual controller. For
  // any other controller type the samples have nowhere to land, so flag the
  // mismatch instead of pretending the touchpad routes.
  if (mode === 'ds4' && (ctrl.controllerType || 'xbox') !== 'playstation') {
    return { cls: 'touchpad-nosurface', text: 'Touchpad: no surface',
             title: 'Touchpad routing is set to "Pad", but this controller’s virtual '
                  + 'device is not a DualShock 4 and has no touchpad surface — the '
                  + 'samples are dropped. Switch this device to "Mouse" or "Off" '
                  + 'under Paired Devices, or pair it as a PlayStation controller.' };
  }
  const dest = (mode === 'mouse') ? 'mouse' : 'pad';
  const destLabel = (mode === 'mouse') ? 'the host mouse pointer' : 'the virtual DualShock 4 touchpad';
  if (!ctrl.touchpadActive) {
    return { cls: 'touchpad-ready', text: 'Touchpad → ' + dest,
             title: 'Touchpad samples will route to ' + destLabel + ' — none received yet.' };
  }
  return { cls: 'touchpad-on', text: 'Touchpad → ' + dest,
           title: 'Touchpad input is streaming to ' + destLabel + '.' };
}

// ── Per-controller lightbar chip (Task 1.4) ─────────────────────────────────
// The host game's lightbar colour, returned to a DualSense / DS4. Derived from
// `ctrl.lightbarCapable` (the CAP_LIGHTBAR bit the sender advertised — also the
// gate the receiver uses to decide whether to emit MSG_LIGHTBAR) and
// `ctrl.lightbar` (the most recent "#rrggbb" colour, or null until the game
// sets one). Mirrors MOTION_COPY's { cls, text, title } shape, plus `swatch`:
// a validated CSS colour shown as a small square, or null when there is none.
function lightbarChip(ctrl) {
  if (!ctrl.lightbarCapable) {
    return { cls: 'lightbar-na', text: 'No lightbar', swatch: null,
             title: 'This controller has no addressable RGB lightbar — an Xbox pad, '
                  + 'or a sender that did not advertise lightbar support. The host '
                  + 'game’s colour is not forwarded to it.' };
  }
  // Only ever trust a strict #rrggbb string into the swatch's style attribute.
  const raw = (typeof ctrl.lightbar === 'string') ? ctrl.lightbar : null;
  const colour = (raw && /^#[0-9a-f]{6}$/i.test(raw)) ? raw : null;
  if (!colour) {
    return { cls: 'lightbar-ready', text: 'Lightbar ready', swatch: null,
             title: 'The controller has an RGB lightbar and the sender accepts the '
                  + 'lightbar return path — waiting for the host game to set a colour.' };
  }
  return { cls: 'lightbar-on', text: 'Lightbar', swatch: colour,
           title: 'The host game is driving this controller’s lightbar — current colour '
                + colour.toUpperCase() + '.' };
}

// ── Inline failure feedback ─────────────────────────────────────────────────
// Surface a dashboard action failure (disconnect, remove device, touchpad-mode
// change) in the shared #dash-notice strip. Consistent with how the Updates
// section flags errors — a visible inline message rather than a silent no-op.
let dashNoticeTimer = null;
function showDashError(msg) {
  const el = document.getElementById('dash-notice');
  const txt = document.getElementById('dash-notice-text');
  if (!el || !txt) return;
  txt.textContent = msg;          // textContent — caller strings are not trusted
  el.classList.add('show');
  if (dashNoticeTimer) clearTimeout(dashNoticeTimer);
  // Auto-dismiss after 8s; the close button clears it sooner.
  dashNoticeTimer = setTimeout(hideDashError, 8000);
}
function hideDashError() {
  const el = document.getElementById('dash-notice');
  if (el) el.classList.remove('show');
  if (dashNoticeTimer) { clearTimeout(dashNoticeTimer); dashNoticeTimer = null; }
}

// Pull a human-readable reason out of an apiPost() result for showDashError().
function apiErrorText(res, fallback) {
  if (res && res.status === 0) return fallback + ' — server unreachable.';
  const detail = (res && res.data && res.data.error) ? res.data.error : null;
  return detail ? fallback + ' — ' + detail + '.' : fallback + '.';
}

// Guards one-time listener wiring — initDashboard() runs on every nav back to
// /dashboard, but the elements it binds (#dash-notice-close, #device-list,
// #connection-list) are static, so their listeners must attach exactly once.
let dashboardListenersWired = false;

function initDashboard() {
  hideDashError();
  if (!dashboardListenersWired) {
    const closeBtn = document.getElementById('dash-notice-close');
    if (closeBtn) closeBtn.addEventListener('click', hideDashError);
    const devList = document.getElementById('device-list');
    if (devList) devList.addEventListener('click', handleDeviceListClick);
    const connList = document.getElementById('connection-list');
    if (connList) connList.addEventListener('click', handleConnectionListClick);
    dashboardListenersWired = true;
  }
  startSSE();
  loadDevices();
  checkBackendStatus();
}

// ── SSE-reconnect bar ──────────────────────────────────────────────────────
// Mounted in #dashboard-sse-reconnect inside index.html. Drawn with the bar
// loader (whole-pane / area-level wait, per the design spec) so the user
// sees the live stream is mid-reconnect rather than the dashboard silently
// going stale. The bar mounts lazily on first need; once mounted, we just
// flip `hidden` on the wrapper.
function setSseReconnecting(on) {
  const slot = document.getElementById('dashboard-sse-reconnect');
  if (!slot) return;
  if (on) {
    if (!slot.firstElementChild) slot.innerHTML = barSVG(240);
    slot.hidden = false;
  } else {
    slot.hidden = true;
  }
}

// ── SSE (replaces polling) ──────────────────────────────────────────────────
function startSSE() {
  stopSSE();
  eventSource = new EventSource('/api/events');

  // Any successful event delivery means the stream is healthy again — clear
  // the reconnect bar if it was shown by a prior onerror.
  const onAnyMessage = () => setSseReconnecting(false);

  eventSource.addEventListener('status', (e) => {
    onAnyMessage();
    try {
      const d = JSON.parse(e.data);
      updateStatus(d);
    } catch (err) { /* ignore */ }
  });

  eventSource.addEventListener('connections', (e) => {
    onAnyMessage();
    try {
      const d = JSON.parse(e.data);
      updateConnections(d);
      // Capture active-connection count for the restart-confirmation modal.
      window.__activeConnectionCount = (d.connections || []).length;
      // Keep the paired-device touchpad segmented control in sync. The SSE
      // stream only pushes `connections`, so a touchpad-mode change made in
      // another tab would otherwise leave the segmented control stale while
      // the per-controller chip (SSE-fed) updates — the two would visibly
      // disagree. Re-render the device list from /api/devices on each tick.
      loadDevices();
    } catch (err) { /* ignore */ }
  });

  eventSource.addEventListener('update', (e) => {
    onAnyMessage();
    try {
      const d = JSON.parse(e.data);
      if (typeof updatesHandleSSE === 'function') updatesHandleSSE(d);
    } catch (err) { /* ignore */ }
  });

  // PIN state — { state: "idle|active|expired|paired", secondsRemaining }.
  // Drives the "Expires in m:ss" countdown on the PIN panel and the brief
  // "Paired!" flash after a successful verifyPin().
  eventSource.addEventListener('pin', (e) => {
    onAnyMessage();
    try {
      const d = JSON.parse(e.data);
      updatePinPanel(d);
    } catch (err) { /* ignore */ }
  });

  eventSource.onerror = () => {
    stopSSE();
    // Check if server is truly unreachable vs just a transient SSE glitch
    fetch('/api/status', { signal: AbortSignal.timeout(3000) })
      .then(r => {
        if (r.ok) {
          // Server is still up, just SSE dropped — reconnect. Surface the
          // bar loader at the top of the dashboard for the gap so the user
          // can see the live stream is mid-recovery rather than the page
          // silently going stale.
          setSseReconnecting(true);
          setTimeout(startSSE, 2000);
        } else {
          // Truly offline — the offline view takes over the whole card,
          // so any reconnect bar that was on screen is now hidden by the
          // showView() swap. Clear the flag so it doesn't briefly flash
          // when we come back online.
          setSseReconnecting(false);
          showOffline();
        }
      })
      .catch(() => {
        // Server unreachable
        setSseReconnecting(false);
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

  // Update backend status from SSE data if available
  if (d.backend) {
    updateBackendPanel(d.backend, d.backendAvailable);
  }
}

// Replace an element's markup only when it changed. The controller list
// re-renders on every SSE `connections` tick, but its contents change far
// less often — skipping no-op writes keeps the SMIL-animated charging-battery
// icon from restarting its loop on each tick.
function setHTML(el, html) {
  if (el.__html === html) return;
  el.__html = html;
  el.innerHTML = html;
}

function updateConnections(d) {
  const connEl = document.getElementById('connection-list');
  const ctrlEl = document.getElementById('controller-list');
  const countEl = document.getElementById('controller-count');

  if (!d.connections || d.connections.length === 0) {
    if (connEl) connEl.innerHTML = '<p class="hint">No active connections</p>';
    if (ctrlEl) setHTML(ctrlEl, '<p class="hint">No active controllers</p>');
    if (countEl) countEl.textContent = '0 / ' + (d.maxControllers || 16);
    return;
  }

  // ── Connections list (network sessions) ──
  // Disconnect rides in a data-* attribute + delegated handler rather than an
  // inline onclick — the connectionId is never spliced into a JS-string
  // context (esc() only escapes for the HTML-attribute context).
  // The `device-state` chip text comes from DEVICE_LINK_STATE_LABEL keyed on
  // the server's lowercase `state` string (defaults to "Online" — a row that
  // shows up in /api/connections at all is at least Active).
  if (connEl) {
    connEl.innerHTML = d.connections.map(c => {
      const stateKey = c.state || 'active';
      const stateText = DEVICE_LINK_STATE_LABEL[stateKey] || DEVICE_LINK_STATE_LABEL.active;
      return `
      <div class="device-item">
        <div class="device-info">
          <span class="device-name">${esc(c.deviceName)} <span class="device-state state-${esc(stateKey)}">${esc(stateText)}</span></span>
          <span class="device-meta">${esc(c.senderIP)} · ${c.activeControllerCount || 0} controller${(c.activeControllerCount||0) === 1 ? '' : 's'}</span>
        </div>
        <button class="btn-icon btn-danger" type="button" data-act="disconnect" data-conn-id="${esc(c.connectionId)}" title="Disconnect"><img src="img/icons/close_x.svg" alt="Disconnect" class="emoji-icon"></button>
      </div>`;
    }).join('');
  }

  // ── Virtual controllers list (per-device ViGEm state) ──
  if (ctrlEl) {
    const allCtrls = [];
    d.connections.forEach(c => {
      c.controllers.forEach(ctrl => {
        allCtrls.push({ ...ctrl, deviceName: c.deviceName, connectionId: c.connectionId,
                        touchpadMode: c.touchpadMode });
      });
    });
    if (allCtrls.length === 0) {
      setHTML(ctrlEl, '<p class="hint">No active controllers</p>');
    } else {
      setHTML(ctrlEl, allCtrls.map(ctrl => {
        const ok = ctrl.pluggedIn;
        const ctrlType = ctrl.controllerType || 'xbox';
        const ctrlLabel = ctrl.controllerTypeLabel || 'Xbox';
        const m = MOTION_COPY[motionStateId(ctrl)] || MOTION_COPY.na;
        const bat = batteryChip(ctrl);
        const tp = touchpadChip(ctrl);
        const lb = lightbarChip(ctrl);
        // Per-controller pipeline state — falls back to "Mounted" for an
        // active controller and "Blocked" for an inactive one (the server
        // only stamps live/detached today; see ControllerState in types.h).
        const stateKey = ctrl.state || (ok ? 'live' : 'failed');
        const stateText = CONTROLLER_STATE_LABEL[stateKey]
                        || (ok ? CONTROLLER_STATE_LABEL.live : CONTROLLER_STATE_LABEL.failed);
        return `
        <div class="ctrl-item">
          <div class="ctrl-row">
            <img class="ctrl-type-icon" src="img/ctrl-${esc(ctrlType)}.svg" alt="${esc(ctrlLabel)}" title="${esc(ctrlLabel)}">
            <div class="ctrl-info">
              <span class="ctrl-name"><span class="ctrl-dot ${ok ? 'ok' : 'err'}"></span>Controller #${ctrl.controllerIndex} · ${esc(ctrlLabel)}</span>
              <span class="ctrl-meta">${esc(ctrl.deviceName)} · Serial ${ctrl.serialNo} · <span class="ctrl-state state-${esc(stateKey)}">${esc(stateText)}</span></span>
            </div>
          </div>
          <div class="ctrl-chips">
            <span class="ctrl-battery ${bat.cls}" title="${esc(bat.title)}"><img class="ctrl-battery-icon" src="img/icons/${esc(bat.icon)}" alt="">${esc(bat.text)}</span>
            <span class="ctrl-motion ${m.cls}" title="${esc(m.title)}">${esc(m.text)}</span>
            <span class="ctrl-touchpad ${tp.cls}" title="${esc(tp.title)}">${esc(tp.text)}</span>
            <span class="ctrl-lightbar ${lb.cls}" title="${esc(lb.title)}">${lb.swatch ? `<span class="lightbar-swatch" style="background:${esc(lb.swatch)}"></span>` : ''}${esc(lb.text)}</span>
          </div>
        </div>`;
      }).join(''));
    }
  }

  if (countEl) countEl.textContent = d.totalControllers + ' / ' + d.maxControllers;
}

async function poll() {
  try {
    const r = await fetch('/api/status');
    const d = await r.json();
    updateStatus(d);
  } catch (e) {
    // Server unreachable during poll
    showOffline();
    return;
  }
  checkBackendStatus();
}

// ── Connections ─────────────────────────────────────────────────────────────
async function disconnectConn(connId, btn) {
  // Per-row disconnect — DELETE /api/connections/<token> is ~0.5s. Replace
  // the close-X glyph with a spinner inside the button while we wait so the
  // user gets immediate feedback the request is in flight; the row itself
  // disappears when the SSE `connections` tick arrives.
  const restore = setButtonLoading(btn);
  try {
    const res = await api('/api/connections/' + encodeURIComponent(connId),
                          { method: 'DELETE' });
    if (!res.ok) {
      showDashError(apiErrorText(res, 'Could not disconnect that connection'));
      restore();
      return;
    }
    hideDashError();
    // Do NOT restore() on success — the row is about to be removed by the
    // SSE `connections` tick, so the original glyph would briefly flash back
    // before the row vanishes. Leaving the spinner running is fine: the
    // node is on its way out within ~500ms.
  } catch (e) {
    restore();
  }
}

// Delegated click handler for the connections list — keeps connectionIds out
// of inline onclick= JS-string contexts.
function handleConnectionListClick(e) {
  const btn = e.target.closest('[data-act="disconnect"]');
  if (!btn) return;
  disconnectConn(btn.getAttribute('data-conn-id') || '', btn);
}

// ── PIN ─────────────────────────────────────────────────────────────────────
// Triggered by the inline onclick on the Generate PIN button. The fetch is
// ~0.2s but user-facing, so we surface the in-flight state with the
// in-button spinner per the design spec rather than letting the button
// look idle until the response lands.
async function genPin(ev) {
  // `event` is the global fallback for older inline-handler call sites; we
  // also accept an explicit argument in case the call site is reworked.
  const btn = (ev && ev.currentTarget) ||
              (typeof event !== 'undefined' && event ? event.currentTarget : null);
  const restore = setButtonLoading(btn, 'Generating…');
  try {
    const { ok, data } = await apiPost('/api/pin/generate');
    document.getElementById('pin-display').textContent = (ok && data.pin) ? data.pin : '—';
    // genPin() returns the PIN once. updatePinPanel() takes over the
    // "Expires in m:ss" countdown from the SSE `pin` stream on the next tick.
  } finally {
    restore();
  }
}

// Render PinState into the dashboard's PIN panel. The wire `pin-display`
// element keeps the actual digits (set once by genPin()); the adjacent
// hint line carries the countdown / expiry / "Paired!" flash so the
// transient states don't overwrite the PIN itself.
function updatePinPanel(s) {
  const hint = document.getElementById('pin-hint');
  const disp = document.getElementById('pin-display');
  if (!hint) return;
  const state = (s && s.state) || 'idle';
  switch (state) {
    case 'active': {
      const secs = Math.max(0, parseInt(s.secondsRemaining, 10) || 0);
      const mm = Math.floor(secs / 60);
      const ss = (secs % 60).toString().padStart(2, '0');
      hint.textContent = 'Enter this PIN on the sender to pair. Expires in '
                       + mm + ':' + ss + '.';
      break;
    }
    case 'expired':
      hint.textContent = 'PIN expired — generate a new one to pair another device.';
      if (disp) disp.textContent = '—';
      break;
    case 'paired':
      hint.textContent = 'Paired successfully — that PIN is now spent.';
      if (disp) disp.textContent = '—';
      break;
    case 'idle':
    default:
      hint.textContent = 'Enter this PIN on the sender to pair. Expires in 5 minutes.';
      break;
  }
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
    // Build markup with no inline handlers — device ids ride in data-*
    // attributes (HTML-escaped) and the click logic is wired via
    // addEventListener below, so an id is never interpolated into a JS
    // string context. Selection on the segmented control is also exposed
    // to assistive tech via role="radio" + aria-checked (L-2).
    el.innerHTML = devs.map(d => {
      const tm = d.touchpadMode || 'ds4';
      const seg = TOUCHPAD_MODES.map(mode => {
        const on = tm === mode.id;
        return `<button class="seg-btn${on ? ' seg-on' : ''}" type="button" `
          + `role="radio" aria-checked="${on ? 'true' : 'false'}" `
          + `title="${esc(mode.title)}" data-act="touchpad-mode" `
          + `data-id="${esc(d.id)}" data-mode="${esc(mode.id)}">${esc(mode.label)}</button>`;
      }).join('');
      // Per-device link-state chip — defaults to "Paired" (offline) for a
      // device with no live connection. The chip text comes from the same
      // DEVICE_LINK_STATE_LABEL table the Connections section uses.
      const stateKey = d.state || 'paired';
      const stateText = DEVICE_LINK_STATE_LABEL[stateKey] || DEVICE_LINK_STATE_LABEL.paired;
      return `
      <div class="device-item">
        <div class="device-info">
          <span class="device-name">${esc(d.name)} <span class="device-state state-${esc(stateKey)}">${esc(stateText)}</span></span>
          <span class="device-meta">${esc(d.lastIP)} · ${esc(d.pairedAt)}</span>
        </div>
        <div class="device-actions">
          <span class="seg-label" title="Where this device's DualSense / DS4 touchpad is routed on this host. Applies live — no re-pairing needed.">Touchpad</span>
          <div class="seg" role="group" aria-label="Touchpad routing">${seg}</div>
          <button class="btn-icon btn-danger" type="button" data-act="remove-device" data-id="${esc(d.id)}" title="Remove"><img src="img/icons/close_x.svg" alt="Remove" class="emoji-icon"></button>
        </div>
      </div>`;
    }).join('');
  } catch (e) { /* ignore */ }
}

// Single delegated click handler for the paired-device list — keeps device
// ids out of inline onclick= JS-string contexts (see esc() note: it is an
// HTML escaper, not a JS-string escaper).
function handleDeviceListClick(e) {
  const btn = e.target.closest('[data-act]');
  if (!btn) return;
  const id = btn.getAttribute('data-id') || '';
  if (btn.dataset.act === 'remove-device') {
    removeDevice(id, btn);
  } else if (btn.dataset.act === 'touchpad-mode') {
    if (btn.classList.contains('seg-on')) return;  // already selected
    setTouchpadMode(id, btn.getAttribute('data-mode') || '', btn);
  }
}

async function removeDevice(id, btn) {
  // POST /api/devices/remove is ~0.2s. Same pattern as disconnect — swap the
  // close-X for the spinner inside the icon button, then let the loadDevices()
  // re-render replace the row.
  const restore = setButtonLoading(btn);
  try {
    const res = await apiPost('/api/devices/remove', { id });
    if (!res.ok) {
      showDashError(apiErrorText(res, 'Could not remove that paired device'));
      restore();
      return;
    }
    hideDashError();
    // Don't restore() — loadDevices() rebuilds the list, which removes this
    // button entirely. Restoring would just flash the close-X glyph back.
    loadDevices();
  } catch (e) {
    restore();
  }
}

// Set a paired device's touchpad routing mode. The server persists it and
// hot-applies it to any live connection, so the change needs no re-pairing.
async function setTouchpadMode(id, mode, btn) {
  // Disable the entire .seg group during the POST. The fetch is short
  // (~150ms typical) so a per-button loader inside one segment would feel
  // jittery; greying the whole segmented control reads as "in transit"
  // without re-laying out the row. The CSS `button:disabled` rule (alpha
  // 0.4, pointer-events: none) handles the visual + click-blocking.
  const seg = btn ? btn.closest('.seg') : null;
  const segButtons = seg ? Array.from(seg.querySelectorAll('button')) : [];
  segButtons.forEach(b => { b.disabled = true; });
  if (seg) seg.setAttribute('aria-busy', 'true');
  try {
    const res = await apiPost('/api/devices/touchpad-mode', { id, mode });
    if (!res.ok) {
      // A failed POST leaves the clicked segment un-highlighted — surface why,
      // then re-render so the segmented control snaps back to the server's
      // actual (unchanged) mode rather than sitting in a half-applied look.
      showDashError(apiErrorText(res, 'Could not change touchpad routing'));
      loadDevices();
      return;
    }
    hideDashError();
    loadDevices();
  } finally {
    // loadDevices() rebuilds the segmented control with the new selection,
    // so the disabled flags we set live on stale nodes — clearing them
    // here is harmless but keeps the no-op case (failed re-render) sane.
    segButtons.forEach(b => { b.disabled = false; });
    if (seg) seg.removeAttribute('aria-busy');
  }
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


