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
// Resolved lazily through t() so the active-locale catalog is used. The chip
// vocabulary mirrors dish-android's chip_status_* keys for cross-app consistency.
function deviceLinkStateLabel(key) {
  switch (key) {
    case 'linking':       return t('device.state.connecting');
    case 'active':        return t('device.state.online');
    case 'notResponding': return t('device.state.notresponding');
    case 'paired':
    default:              return t('device.state.paired');
  }
}

// DeviceLinkState → v6 brand dish glyph next to the chip. Mirrors the
// "dish" iconography family in img/icons/ (dish.svg, dish-connected.svg,
// dish-receiving-animated.svg, dish-disabled.svg, dish-off.svg). The dish
// reads as "the sender" — i.e. the remote Dish client — so the receiving
// state animates the inbound-signal arcs and the not-responding state uses
// the slash variant.
const DEVICE_LINK_STATE_ICON = {
  paired:        'dish.svg',
  linking:       'dish-scanning-animated.svg',
  active:        'dish-receiving-animated.svg',
  notResponding: 'dish-off.svg',
};

// ControllerState → per-controller tag (virtual-controllers section).
// Today the server only ever stamps "live" or "detached"; the transient
// states (registering, allocating) and the "failed" case are enumerated for
// when the SessionService starts threading them through. Lookup goes through
// t() so the active locale catalog wins.
function controllerStateLabel(key) {
  switch (key) {
    case 'source':       return t('controller.state.detected');
    case 'registering':
    case 'allocating':   return t('controller.state.mounting');
    case 'detached':     return t('controller.state.unmounting');
    case 'failed':       return t('controller.state.blocked');
    case 'live':
    case 'quiet':
    default:             return t('controller.state.mounted');
  }
}

// ── Backend copy table ──────────────────────────────────────────────────────
// Keyed by (backend.id, errorCode). The C++ server emits structured status;
// this table owns every user-facing string. Adding a new error code on the
// server falls back to a generic message until a matching entry is added here.
//
// Resolved via t() so each backend lookup uses the active locale catalog. The
// commands + URLs stay verbatim (they're literal shell snippets, not copy).
function backendCopy(backendId) {
  if (backendId === 'vigem') {
    return {
      title: t('backend.vigem.title'),
      pipelineLabel: t('backend.vigem.pipeline-label'),
      flowLabel: t('backend.vigem.flow-label'),
      statusActive: t('backend.vigem.status.active'),
      statusIdle: t('backend.vigem.status.idle'),
      statusUnknown: t('backend.vigem.status.unknown'),
      errors: {
        DRIVER_MISSING: {
          title: t('backend.vigem.err.driver-missing.title'),
          body: t('backend.vigem.err.driver-missing.body'),
          steps: [
            { text: t('backend.vigem.err.driver-missing.step1'), url: 'https://github.com/nefarius/ViGEmBus/releases' },
            { text: t('backend.vigem.err.driver-missing.step2') },
            { text: t('backend.vigem.err.driver-missing.step3') },
            { text: t('backend.vigem.err.driver-missing.step4') },
          ],
        },
        BUS_OPEN_FAILED: {
          title: t('backend.vigem.err.bus-open-failed.title'),
          body: t('backend.vigem.err.bus-open-failed.body'),
          steps: [
            { text: t('backend.vigem.err.bus-open-failed.step1'), url: 'https://github.com/nefarius/ViGEmBus/releases' },
            { text: t('backend.vigem.err.bus-open-failed.step2') },
          ],
        },
      },
    };
  }
  if (backendId === 'uinput') {
    return {
      title: t('backend.uinput.title'),
      pipelineLabel: t('backend.uinput.pipeline-label'),
      flowLabel: t('backend.uinput.flow-label'),
      statusActive: t('backend.uinput.status.active'),
      statusIdle: t('backend.uinput.status.idle'),
      statusUnknown: t('backend.uinput.status.unknown'),
      errors: {
        DEVICE_MISSING: {
          title: t('backend.uinput.err.device-missing.title'),
          body: t('backend.uinput.err.device-missing.body'),
          steps: [
            { text: t('backend.uinput.err.device-missing.step1'), command: 'sudo modprobe uinput' },
            { text: t('backend.uinput.err.device-missing.step2'), command: "echo uinput | sudo tee /etc/modules-load.d/satellite.conf" },
            { text: t('backend.uinput.err.device-missing.step3') },
          ],
        },
        PERMISSION_DENIED: {
          title: t('backend.uinput.err.permission-denied.title'),
          body: t('backend.uinput.err.permission-denied.body'),
          steps: [
            { text: t('backend.uinput.err.permission-denied.step1'), command: "echo 'KERNEL==\"uinput\", GROUP=\"input\", MODE=\"0660\"' | sudo tee /etc/udev/rules.d/70-satellite-uinput.rules" },
            { text: t('backend.uinput.err.permission-denied.step2'), command: 'sudo udevadm control --reload-rules && sudo udevadm trigger' },
            { text: t('backend.uinput.err.permission-denied.step3'), command: 'sudo usermod -aG input "$USER"' },
            { text: t('backend.uinput.err.permission-denied.step4') },
          ],
        },
      },
    };
  }
  return {};
}

// Compat shim — debug.js dips into BACKEND_COPY[id] for pipeline labels +
// error titles. Keep a Proxy-like accessor that yields the live, localised
// copy on demand, so the debug page doesn't have to learn about backendCopy().
const BACKEND_COPY = new Proxy({}, {
  get(_, backendId) {
    if (typeof backendId !== 'string') return undefined;
    return backendCopy(backendId);
  },
});

// ── Per-controller motion (IMU) copy ────────────────────────────────────────
// Task 1.1 — gyro/accelerometer. Four states, derived from the controller's
// motionCapable / motionActive / motionSink flags in /api/connections. Like
// BACKEND_COPY above, this table owns every user-facing motion string.
//   na     — controller has no IMU (e.g. an Xbox pad): motion not available
//   ready  — IMU present + advertised, but no motion packet received yet
//   on     — motion streaming AND reaching the OS-level virtual gamepad
//   nosink — motion streaming, but the virtual device exposes no IMU surface
//            to deliver it to (Xbox-typed device, old ViGEmBus, or macOS)
function motionCopy(id) {
  switch (id) {
    case 'ready':
      return { cls: 'motion-ready', text: t('motion.ready.text'),  title: t('motion.ready.title') };
    case 'on':
      return { cls: 'motion-on',    text: t('motion.on.text'),     title: t('motion.on.title') };
    case 'nosink':
      return { cls: 'motion-nosink', text: t('motion.nosink.text'), title: t('motion.nosink.title') };
    case 'na':
    default:
      return { cls: 'motion-na',    text: t('motion.na.text'),     title: t('motion.na.title') };
  }
}

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
      text: t('battery.na.text'),
      title: t('battery.na.title'),
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
      text  = t('battery.charging.text',  [pct]);
      title = t('battery.charging.title', [pct]);
      break;
    case 'full':
      text  = t('battery.full.text', [lvl === null ? t('battery.full.label') : pct]);
      title = t('battery.full.title');
      break;
    case 'wired':
      text  = (lvl === null) ? t('battery.wired.ac.text') : t('battery.wired.text', [pct]);
      title = t('battery.wired.title');
      break;
    case 'discharging':
      text  = t('battery.discharging.text',  [pct]);
      title = t('battery.discharging.title', [pct]);
      break;
    default:
      text  = t('battery.unknown.text',  [pct]);
      title = t('battery.unknown.title', [pct]);
  }
  return { cls, text, title, icon: batteryIconFile(b, lvl) };
}

// ── Per-device touchpad routing (Task 1.3) ──────────────────────────────────
// The paired-device touchpad-mode selector. Mirrors TOUCHPAD_MODE_* on the
// server; the chosen mode is persisted in config and hot-applied to any live
// connection (no re-pairing). Pad → virtual DualShock 4 touchpad surface;
// Mouse → relative mouse pointer on this host; Off → ignore.
function touchpadModes() {
  return [
    { id: 'ds4',   label: t('devices.touchpad.pad'),   title: t('devices.touchpad.pad.tip') },
    { id: 'mouse', label: t('devices.touchpad.mouse'), title: t('devices.touchpad.mouse.tip') },
    { id: 'off',   label: t('devices.touchpad.off'),   title: t('devices.touchpad.off.tip') },
  ];
}

// Per-controller touchpad chip — derived from the controller's `touchpadActive`
// flag, its `controllerType`, and the owning connection's `touchpadMode`.
// Mirrors MOTION_COPY's { cls, text, title } shape.
function touchpadChip(ctrl) {
  const mode = ctrl.touchpadMode || 'ds4';
  if (mode === 'off') {
    return { cls: 'touchpad-off', text: t('touchpad.off.text'), title: t('touchpad.off.title') };
  }
  // "Pad" mode forwards into the virtual DualShock 4 touchpad surface — but
  // that surface only exists on a PlayStation-typed virtual controller. For
  // any other controller type the samples have nowhere to land, so flag the
  // mismatch instead of pretending the touchpad routes.
  if (mode === 'ds4' && (ctrl.controllerType || 'xbox') !== 'playstation') {
    return { cls: 'touchpad-nosurface', text: t('touchpad.nosurface.text'), title: t('touchpad.nosurface.title') };
  }
  const dest = (mode === 'mouse') ? t('touchpad.dest.mouse') : t('touchpad.dest.pad');
  const destLabel = (mode === 'mouse') ? t('touchpad.dest.mouse-long') : t('touchpad.dest.pad-long');
  if (!ctrl.touchpadActive) {
    return { cls: 'touchpad-ready', text: t('touchpad.ready.text', [dest]),
             title: t('touchpad.ready.title', [destLabel]) };
  }
  return { cls: 'touchpad-on', text: t('touchpad.on.text', [dest]),
           title: t('touchpad.on.title', [destLabel]) };
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
    return { cls: 'lightbar-na', text: t('lightbar.na.text'), swatch: null,
             title: t('lightbar.na.title') };
  }
  // Only ever trust a strict #rrggbb string into the swatch's style attribute.
  const raw = (typeof ctrl.lightbar === 'string') ? ctrl.lightbar : null;
  const colour = (raw && /^#[0-9a-f]{6}$/i.test(raw)) ? raw : null;
  if (!colour) {
    return { cls: 'lightbar-ready', text: t('lightbar.ready.text'), swatch: null,
             title: t('lightbar.ready.title') };
  }
  return { cls: 'lightbar-on', text: t('lightbar.on.text'), swatch: colour,
           title: t('lightbar.on.title', [colour.toUpperCase()]) };
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
  if (res && res.status === 0) return fallback + ' — ' + t('connections.err.server-unreachable') + '.';
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
    const pairList = document.getElementById('pair-request-list');
    if (pairList) pairList.addEventListener('click', handlePairRequestClick);
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

  // Reverse-pairing requests — array of { deviceId, deviceName, clientIP,
  // secondsRemaining }. Drives the accept/deny panel; an empty array hides it.
  eventSource.addEventListener('pairRequests', (e) => {
    onAnyMessage();
    try {
      const d = JSON.parse(e.data);
      updatePairRequests(d);
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
  document.getElementById('s-status').textContent =
    d.listening ? t('dashboard.status.listening') : t('dashboard.status.stopped');
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
    if (connEl) connEl.innerHTML = '<p class="hint">' + esc(t('connections.empty')) + '</p>';
    if (ctrlEl) setHTML(ctrlEl, '<p class="hint">' + esc(t('controllers.empty')) + '</p>');
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
      const stateText = deviceLinkStateLabel(stateKey);
      const stateIcon = DEVICE_LINK_STATE_ICON[stateKey] || DEVICE_LINK_STATE_ICON.active;
      const ctrlCount = c.activeControllerCount || 0;
      const ctrlNoun  = ctrlCount === 1 ? t('connections.controller.singular')
                                        : t('connections.controller.plural');
      const disconnectLabel = t('connections.disconnect');
      return `
      <div class="device-item">
        <img class="device-glyph" src="img/icons/${esc(stateIcon)}" alt="">
        <div class="device-info">
          <span class="device-name">${esc(c.deviceName)} <span class="device-state state-${esc(stateKey)}">${esc(stateText)}</span></span>
          <span class="device-meta">${esc(c.senderIP)} · ${ctrlCount} ${esc(ctrlNoun)}</span>
        </div>
        <button class="btn-icon btn-danger" type="button" data-act="disconnect" data-conn-id="${esc(c.connectionId)}" title="${esc(disconnectLabel)}"><img src="img/icons/close_x.svg" alt="${esc(disconnectLabel)}" class="emoji-icon"></button>
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
      setHTML(ctrlEl, '<p class="hint">' + esc(t('controllers.empty')) + '</p>');
    } else {
      setHTML(ctrlEl, allCtrls.map(ctrl => {
        const ok = ctrl.pluggedIn;
        const ctrlType = ctrl.controllerType || 'xbox';
        const ctrlLabel = ctrl.controllerTypeLabel || 'Xbox';
        const m = motionCopy(motionStateId(ctrl));
        const bat = batteryChip(ctrl);
        const tp = touchpadChip(ctrl);
        const lb = lightbarChip(ctrl);
        // Per-controller pipeline state — falls back to "Mounted" for an
        // active controller and "Blocked" for an inactive one (the server
        // only stamps live/detached today; see ControllerState in types.h).
        const stateKey = ctrl.state || (ok ? 'live' : 'failed');
        const stateText = controllerStateLabel(stateKey);
        const ctrlHeading = t('controllers.controller-num', [ctrl.controllerIndex]);
        const serialLabel = t('controllers.serial');
        return `
        <div class="ctrl-item">
          <div class="ctrl-row">
            <img class="ctrl-type-icon" src="img/ctrl-${esc(ctrlType)}.svg" alt="${esc(ctrlLabel)}" title="${esc(ctrlLabel)}">
            <div class="ctrl-info">
              <span class="ctrl-name"><span class="ctrl-dot ${ok ? 'ok' : 'err'}"></span>${esc(ctrlHeading)} · ${esc(ctrlLabel)}</span>
              <span class="ctrl-meta">${esc(ctrl.deviceName)} · ${esc(serialLabel)} ${ctrl.serialNo} · <span class="ctrl-state state-${esc(stateKey)}">${esc(stateText)}</span></span>
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
      showDashError(apiErrorText(res, t('connections.err.disconnect')));
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
  const restore = setButtonLoading(btn, t('pin.generating'));
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
      hint.textContent = t('pin.hint.active', [mm + ':' + ss]);
      break;
    }
    case 'expired':
      hint.textContent = t('pin.hint.expired');
      if (disp) disp.textContent = '—';
      break;
    case 'paired':
      hint.textContent = t('pin.hint.paired');
      if (disp) disp.textContent = '—';
      break;
    case 'idle':
    default:
      hint.textContent = t('pin.hint.idle');
      break;
  }
}

// ── Reverse-direction pairing (the dish shows a PIN; the operator accepts) ───
// Render one row per in-flight request with a box to type the dish's PIN into.
// The server never sends that PIN, so the input starts empty — typing it is
// what authenticates the device. We preserve a half-typed value across the
// per-second SSE re-render so the operator isn't interrupted mid-entry.
function updatePairRequests(list) {
  const section = document.getElementById('pair-request-section');
  const el = document.getElementById('pair-request-list');
  if (!section || !el) return;

  const reqs = Array.isArray(list) ? list : [];
  if (reqs.length === 0) {
    section.style.display = 'none';
    el.innerHTML = '';
    return;
  }
  section.style.display = '';

  const typed = {};
  el.querySelectorAll('[data-pair-pin]').forEach(inp => {
    typed[inp.getAttribute('data-pair-pin')] = inp.value;
  });

  const acceptLabel = t('pairreq.accept');
  const denyLabel = t('pairreq.deny');
  const pinPlaceholder = t('pairreq.pin.placeholder');
  el.innerHTML = reqs.map(r => {
    const secs = Math.max(0, parseInt(r.secondsRemaining, 10) || 0);
    const mm = Math.floor(secs / 60);
    const ss = (secs % 60).toString().padStart(2, '0');
    const prior = (typed[r.deviceId] != null) ? typed[r.deviceId] : '';
    return `
      <div class="device-item pair-request-item">
        <img class="device-glyph" src="img/icons/dish-scanning-animated.svg" alt="">
        <div class="device-info">
          <span class="device-name">${esc(r.deviceName || r.deviceId)}</span>
          <span class="device-meta">${esc(r.clientIP)} · ${esc(t('pairreq.expires', [mm + ':' + ss]))}</span>
        </div>
        <div class="device-actions">
          <input type="text" inputmode="numeric" autocomplete="off" maxlength="8" style="width:120px"
                 class="pair-pin-input" data-pair-pin="${esc(r.deviceId)}"
                 placeholder="${esc(pinPlaceholder)}" value="${esc(prior)}">
          <button class="btn btn-save" type="button" data-act="pair-accept" data-id="${esc(r.deviceId)}">${esc(acceptLabel)}</button>
          <button class="btn-icon btn-danger" type="button" data-act="pair-deny" data-id="${esc(r.deviceId)}" title="${esc(denyLabel)}"><img src="img/icons/close_x.svg" alt="${esc(denyLabel)}" class="emoji-icon"></button>
        </div>
      </div>`;
  }).join('');
}

// Delegated click handler for the pairing-request list. Accept reads the PIN
// from the row's own input (never a global selector) so two concurrent
// requests can't cross-wire their PINs.
function handlePairRequestClick(e) {
  const btn = e.target.closest('[data-act]');
  if (!btn) return;
  const id = btn.getAttribute('data-id') || '';
  if (btn.dataset.act === 'pair-deny') {
    respondPairRequest(id, '', false, btn);
    return;
  }
  if (btn.dataset.act === 'pair-accept') {
    const row = btn.closest('.pair-request-item');
    const input = row ? row.querySelector('.pair-pin-input') : null;
    const pin = input ? input.value.trim() : '';
    if (!pin) {
      showDashError(t('pairreq.err.pin-required'));
      if (input) input.focus();
      return;
    }
    respondPairRequest(id, pin, true, btn);
  }
}

async function respondPairRequest(deviceId, pin, accept, btn) {
  const restore = setButtonLoading(btn, accept ? t('pairreq.accepting') : null);
  try {
    const res = await apiPost('/api/pair/respond', { deviceId, pin, accept });
    // A PIN mismatch comes back HTTP-200 with ok:false, so check both layers.
    if (!res.ok || (res.data && res.data.ok === false)) {
      showDashError(apiErrorText(res, accept ? t('pairreq.err.accept') : t('pairreq.err.deny')));
      restore();
      return;
    }
    hideDashError();
    // Leave the spinner — the SSE pairRequests tick drops this row within ~1s.
  } catch (e) {
    restore();
  }
}

// ── Devices ─────────────────────────────────────────────────────────────────
async function loadDevices() {
  try {
    // First time round, populate the capabilities cache so the touchpad
    // badge knows which modes this host can honour. Cheap (one fetch) and
    // server-side state-free — the result lives in g_serverCapabilities.
    if (g_serverCapabilities == null) await loadServerCapabilities();
    const r = await fetch('/api/devices');
    if (!r.ok) return;
    const devs = await r.json();
    const el = document.getElementById('device-list');
    if (devs.length === 0) {
      el.innerHTML = '<p class="hint">' + esc(t('devices.empty')) + '</p>';
      return;
    }
    // Build markup with no inline handlers — device ids ride in data-*
    // attributes (HTML-escaped) and the click logic is wired via
    // addEventListener below, so an id is never interpolated into a JS
    // string context. Selection on the segmented control is also exposed
    // The client (dish app) owns the touchpad mode setting; the dashboard is
    // a read-only mirror. Show the current mode as a badge plus a hint about
    // which modes this host supports — the client picker greys out the rest.
    const modes = touchpadModes();
    const tpLabel  = t('devices.touchpad.label');
    const tpTip    = t('devices.touchpad.tip');
    const removeLb = t('devices.remove');
    const supportedModes = (g_serverCapabilities && g_serverCapabilities.touchpad &&
                            Array.isArray(g_serverCapabilities.touchpad.supportedModes))
      ? g_serverCapabilities.touchpad.supportedModes
      : ['off'];
    const supportedLabels = modes
      .filter(m => supportedModes.indexOf(m.id) !== -1)
      .map(m => m.label).join(' · ');
    el.innerHTML = devs.map(d => {
      const tm = d.touchpadMode || 'off';
      const modeMeta = modes.find(m => m.id === tm) || modes[modes.length - 1];
      const supportedHere = supportedModes.indexOf(tm) !== -1;
      const badgeCls = supportedHere ? `tp-badge tp-badge-${esc(tm)}` : 'tp-badge tp-badge-unsupported';
      // Per-device link-state chip — defaults to "Paired" (offline) for a
      // device with no live connection. Reuses the same vocabulary the
      // Connections section uses (dish-android chip_status_* family).
      const stateKey = d.state || 'paired';
      const stateText = deviceLinkStateLabel(stateKey);
      const stateIcon = DEVICE_LINK_STATE_ICON[stateKey] || DEVICE_LINK_STATE_ICON.paired;
      return `
      <div class="device-item">
        <img class="device-glyph" src="img/icons/${esc(stateIcon)}" alt="">
        <div class="device-info">
          <span class="device-name">${esc(d.name)} <span class="device-state state-${esc(stateKey)}">${esc(stateText)}</span></span>
          <span class="device-meta">${esc(d.lastIP)} · ${esc(d.pairedAt)}</span>
        </div>
        <div class="device-actions">
          <span class="seg-label" title="${esc(tpTip)}">${esc(tpLabel)}</span>
          <span class="${badgeCls}" title="${esc(t('devices.touchpad.client-set'))} — ${esc(t('devices.touchpad.supported-here'))}: ${esc(supportedLabels)}">${esc(modeMeta.label)}</span>
          <button class="btn-icon btn-danger" type="button" data-act="remove-device" data-id="${esc(d.id)}" title="${esc(removeLb)}"><img src="img/icons/close_x.svg" alt="${esc(removeLb)}" class="emoji-icon"></button>
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
  }
  // Touchpad mode is client-owned; the dashboard reflects state only — no
  // segment buttons to click.
}

async function removeDevice(id, btn) {
  // POST /api/devices/remove is ~0.2s. Same pattern as disconnect — swap the
  // close-X for the spinner inside the icon button, then let the loadDevices()
  // re-render replace the row.
  const restore = setButtonLoading(btn);
  try {
    const res = await apiPost('/api/devices/remove', { id });
    if (!res.ok) {
      showDashError(apiErrorText(res, t('devices.err.remove')));
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

// Cached server capabilities — populated lazily on first dashboard load.
// Drives which touchpad modes the read-only badge knows are honoured here;
// the actual selection is owned by the client (dish app), not the dashboard.
let g_serverCapabilities = null;

async function loadServerCapabilities() {
  try {
    const res = await fetch('/api/server/capabilities');
    if (res.ok) {
      g_serverCapabilities = await res.json();
    }
  } catch (e) { /* leave g_serverCapabilities null — UI falls back to "off only" */ }
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

  const copy = backendCopy(backend.id);
  const titleEl  = document.getElementById('backend-title');
  const flowText = document.getElementById('flow-backend-text');
  if (titleEl)  titleEl.textContent  = copy.title || t('backend.section.title');
  if (flowText) flowText.textContent = copy.flowLabel || backend.id;

  const dot     = document.getElementById('backend-dot');
  const label   = document.getElementById('backend-label');
  const toggle  = document.getElementById('backend-guide-toggle');
  const flowBe  = document.getElementById('flow-backend');
  const flowSys = document.getElementById('flow-system');
  const guide   = document.getElementById('backend-guide');

  if (backend.available) {
    dot.className   = 'backend-dot backend-ok';
    label.textContent = backendActive ? (copy.statusActive || t('debug.status.active'))
                                      : (copy.statusIdle   || t('debug.status.idle'));
    label.className = 'backend-label backend-ok-text';
    toggle.style.display = 'none';
    flowBe.className  = 'flow-step done';
    flowSys.className = 'flow-step ' + (backendActive ? 'done' : '');
    guide.style.display = 'none';
    backendGuideOpen = false;
  } else {
    dot.className   = 'backend-dot backend-err';
    const err = (copy.errors && copy.errors[backend.errorCode]) || null;
    label.textContent = err ? err.title : t('backend.unavailable');
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
    titleEl.textContent = t('backend.guide.none.title');
    bodyEl.textContent  = t('backend.guide.none.body');
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
  const label = esc(t('backend.guide.toggle'));
  btn.innerHTML = backendGuideOpen
    ? '<span>' + label + '</span> <img src="img/icons/chevron_down.svg" alt="" class="emoji-icon">'
    : '<span>' + label + '</span> <img src="img/icons/chevron_right.svg" alt="" class="emoji-icon">';
}


