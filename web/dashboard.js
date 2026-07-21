let eventSource = null;

// Disable a button and swap its contents for a spinner (+ optional label).
// Returns a restorer to call once the request settles.
function setButtonLoading(btn, label) {
  if (!btn) return function () {};
  const prevHTML     = btn.innerHTML;
  const prevDisabled = btn.disabled;
  const prevAria     = btn.getAttribute('aria-busy');
  btn.disabled = true;
  btn.setAttribute('aria-busy', 'true');
  // 12px fits inside the 32x32 .btn-icon; 14px reads next to a text label.
  const size = btn.classList.contains('btn-icon') ? 12 : 14;
  if (label) {
    btn.innerHTML = '<span class="btn-with-loader">' + spinnerSVG(size) + '<span>' + esc(label) + '</span></span>';
  } else {
    btn.innerHTML = '<span class="btn-with-loader">' + spinnerSVG(size) + '</span>';
  }
  return function restore() {
    btn.innerHTML = prevHTML;
    btn.disabled  = prevDisabled;
    if (prevAria === null) btn.removeAttribute('aria-busy');
    else                   btn.setAttribute('aria-busy', prevAria);
  };
}

// Map the server's wire `state` strings (mirrors core/types.h) to chip text.
// "linking" is never surfaced today (the handshake is synchronous) but is kept
// for forward-compat.
function deviceLinkStateLabel(key) {
  switch (key) {
    case 'linking':       return t('device.state.connecting');
    case 'active':        return t('device.state.online');
    case 'notResponding': return t('device.state.notresponding');
    case 'paired':
    default:              return t('device.state.paired');
  }
}

const DEVICE_LINK_STATE_ICON = {
  paired:        'dish.svg',
  linking:       'dish-scanning-animated.svg',
  active:        'dish-receiving-animated.svg',
  notResponding: 'dish-off.svg',
};

// The server only stamps "live" or "detached" today; the transient and
// "failed" cases are enumerated for when SessionService threads them through.
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

// Keyed by (backend.id, errorCode); a new server error code falls back to a
// generic message until an entry is added here. Commands and URLs stay
// verbatim (literal shell snippets, not copy).
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
  if (backendId === 'machid') {
    return {
      title: t('backend.machid.title'),
      pipelineLabel: t('backend.machid.pipeline-label'),
      flowLabel: t('backend.machid.flow-label'),
      statusActive: t('backend.machid.status.active'),
      statusIdle: t('backend.machid.status.idle'),
      statusUnknown: t('backend.machid.status.unknown'),
      // No remediation table: an unentitled satellite reports the `none`
      // backend (panel hidden) instead of machid-with-errorCode, so machid
      // never surfaces in the unavailable panel.
      errors: {},
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

// Compat shim: debug.js indexes BACKEND_COPY[id] for pipeline labels + error
// titles. This Proxy yields live localised copy on demand.
const BACKEND_COPY = new Proxy({}, {
  get(_, backendId) {
    if (typeof backendId !== 'string') return undefined;
    return backendCopy(backendId);
  },
});

// Motion (IMU) states derived from motionCapable/motionActive/motionSink:
//   na     no IMU (e.g. Xbox pad)
//   ready  IMU present + advertised, no packet yet
//   on     streaming and reaching the virtual gamepad
//   nosink streaming, but the virtual device has no IMU surface to receive it
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

function motionStateId(ctrl) {
  if (!ctrl.motionCapable) return 'na';
  if (!ctrl.motionActive) return 'ready';
  return ctrl.motionSink ? 'on' : 'nosink';
}

// Status drives the charging icons; otherwise level picks a rung of the
// charge ladder (mirrors the battery asset USAGE.md).
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

// Derived from ctrl.battery ({ level: 0..100|null, status } or null). A wired
// controller falls back to the host machine's battery (100% on a desktop).
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

// Derived from touchpadActive, controllerType, and touchpadMode (client-owned;
// the dashboard only mirrors it).
function touchpadChip(ctrl) {
  const mode = ctrl.touchpadMode || 'ds4';
  if (mode === 'off') {
    return { cls: 'touchpad-off', text: t('touchpad.off.text'), title: t('touchpad.off.title') };
  }
  // "Pad" mode needs the virtual DualShock 4 touchpad surface, which only
  // exists on a PlayStation-typed controller; flag the mismatch otherwise.
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

// Derived from ctrl.lightbarCapable (CAP_LIGHTBAR) and ctrl.lightbar (latest
// "#rrggbb", or null). `swatch` is a validated colour or null.
function lightbarChip(ctrl) {
  if (!ctrl.lightbarCapable) {
    return { cls: 'lightbar-na', text: t('lightbar.na.text'), swatch: null,
             title: t('lightbar.na.title') };
  }
  // Only trust a strict #rrggbb string into the swatch's style attribute.
  const raw = (typeof ctrl.lightbar === 'string') ? ctrl.lightbar : null;
  const colour = (raw && /^#[0-9a-f]{6}$/i.test(raw)) ? raw : null;
  if (!colour) {
    return { cls: 'lightbar-ready', text: t('lightbar.ready.text'), swatch: null,
             title: t('lightbar.ready.title') };
  }
  return { cls: 'lightbar-on', text: t('lightbar.on.text'), swatch: colour,
           title: t('lightbar.on.title', [colour.toUpperCase()]) };
}

let dashNoticeTimer = null;
function showDashError(msg) {
  const el = document.getElementById('dash-notice');
  const txt = document.getElementById('dash-notice-text');
  if (!el || !txt) return;
  txt.textContent = msg;          // textContent: caller strings are untrusted
  el.classList.add('show');
  if (dashNoticeTimer) clearTimeout(dashNoticeTimer);
  dashNoticeTimer = setTimeout(hideDashError, 8000);
}
function hideDashError() {
  const el = document.getElementById('dash-notice');
  if (el) el.classList.remove('show');
  if (dashNoticeTimer) { clearTimeout(dashNoticeTimer); dashNoticeTimer = null; }
}

function apiErrorText(res, fallback) {
  if (res && res.status === 0) return fallback + ': ' + t('connections.err.server-unreachable') + '.';
  const detail = (res && res.data && res.data.error) ? res.data.error : null;
  return detail ? fallback + ': ' + detail + '.' : fallback + '.';
}

// initDashboard() runs on every nav to /dashboard, but the elements it binds
// are static, so attach their listeners exactly once.
let dashboardListenersWired = false;

function initDashboard() {
  hideDashError();
  if (!dashboardListenersWired) {
    const closeBtn = document.getElementById('dash-notice-close');
    if (closeBtn) closeBtn.addEventListener('click', hideDashError);
    const devList = document.getElementById('device-list');
    if (devList) devList.addEventListener('click', handleDeviceListClick);
    const pairList = document.getElementById('pair-request-list');
    if (pairList) pairList.addEventListener('click', handlePairRequestClick);
    const pairBadge = document.getElementById('pair-badge');
    if (pairBadge) {
      pairBadge.addEventListener('click', () => {
        const sec = document.getElementById('pair-request-section');
        if (sec) sec.scrollIntoView({ behavior: 'smooth', block: 'start' });
      });
    }
    dashboardListenersWired = true;
  }
  startSSE();
  loadDevices();
  checkBackendStatus();
  dashRenderNetWarning();
}

async function dashRenderNetWarning() {
  const el = document.getElementById('dash-net-notice');
  if (!el) return;
  const d = await getNetInfo();
  let msg = '';
  if (d && d.category === 'public' && !d.allowPublic) {
    msg = t('dash.net.public');
  } else if (d && d.firewall && d.firewall.supported &&
             (d.firewall.state === 'missing' || d.firewall.state === 'wrong-profile')) {
    msg = t('dash.firewall.warn');
  }
  if (msg) {
    el.innerHTML = esc(msg) +
      ' <a href="/settings" onclick="navigate(\'/settings\');return false;">' +
      esc(t('dash.net.open-settings')) + '</a>';
    el.classList.add('show');
  } else {
    el.classList.remove('show');
    el.innerHTML = '';
  }
}

// Shows the bar loader while the live stream reconnects, so the dashboard
// doesn't silently go stale. Mounts lazily on first need.
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

function startSSE() {
  stopSSE();
  eventSource = new EventSource('/api/events');

  // Any successful delivery means the stream recovered; clear the bar.
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
      g_lastConnections = d;
      updateConnections(d);
      renderDeviceList();
      window.__activeConnectionCount = (d.connections || []).length;
    } catch (err) { /* ignore */ }
  });

  // The device list needs both this and `connections` to render one row each.
  eventSource.addEventListener('devices', (e) => {
    onAnyMessage();
    try {
      g_lastDevices = JSON.parse(e.data);
      renderDeviceList();
    } catch (err) { /* ignore */ }
  });

  eventSource.addEventListener('update', (e) => {
    onAnyMessage();
    try {
      const d = JSON.parse(e.data);
      if (typeof updatesHandleSSE === 'function') updatesHandleSSE(d);
    } catch (err) { /* ignore */ }
  });

  eventSource.addEventListener('pin', (e) => {
    onAnyMessage();
    try {
      const d = JSON.parse(e.data);
      updatePinPanel(d);
    } catch (err) { /* ignore */ }
  });

  eventSource.addEventListener('pairRequests', (e) => {
    onAnyMessage();
    try {
      const d = JSON.parse(e.data);
      updatePairRequests(d);
    } catch (err) { /* ignore */ }
  });

  eventSource.onerror = () => {
    stopSSE();
    // Distinguish a transient SSE glitch from a truly unreachable server.
    fetch('/api/status', { signal: AbortSignal.timeout(3000) })
      .then(r => {
        if (r.ok) {
          setSseReconnecting(true);
          setTimeout(startSSE, 2000);
        } else {
          setSseReconnecting(false);
          showOffline();
        }
      })
      .catch(() => {
        setSseReconnecting(false);
        showOffline();
      });
  };

  poll();
}

function stopSSE() {
  if (eventSource) { eventSource.close(); eventSource = null; }
}

function updateStatus(d) {
  if (d.backend) {
    updateBackendPanel(d.backend, d.backendAvailable);
  }
}

// Write markup only when it changed, else the SMIL charging-battery icon
// restarts its loop on every SSE tick.
function setHTML(el, html) {
  if (el.__html === html) return;
  el.__html = html;
  el.innerHTML = html;
}

function updateConnections(d) {
  const ctrlEl = document.getElementById('controller-list');
  const countEl = document.getElementById('controller-count');

  if (!d.connections || d.connections.length === 0) {
    if (ctrlEl) setHTML(ctrlEl, '<p class="hint">' + esc(t('controllers.empty')) + '</p>');
    if (countEl) countEl.textContent = '0 / ' + (d.maxControllers || 16);
    return;
  }

  if (ctrlEl) {
    const allCtrls = [];
    d.connections.forEach(c => {
      c.controllers.forEach(ctrl => {
        allCtrls.push({ ...ctrl, deviceName: c.deviceName, connectionId: c.connectionId,
                        connState: c.state || 'active' });
      });
    });
    if (allCtrls.length === 0) {
      setHTML(ctrlEl, '<p class="hint">' + esc(t('controllers.empty')) + '</p>');
    } else {
      setHTML(ctrlEl, allCtrls.map(ctrl => {
        // pluggedIn is adapter truth (a virtual device exists now), not an
        // inference from the serial number.
        const ok = ctrl.pluggedIn;
        const ctrlType = ctrl.controllerType || 'xbox';
        const ctrlLabel = ctrl.controllerTypeLabel || 'Xbox';
        const m = motionCopy(motionStateId(ctrl));
        const bat = batteryChip(ctrl);
        const tp = touchpadChip(ctrl);
        const lb = lightbarChip(ctrl);
        // Falls back to live/failed since the server only stamps live/detached
        // today (see ControllerState in types.h).
        const stateKey = ctrl.state || (ok ? 'live' : 'failed');
        const stateText = controllerStateLabel(stateKey);
        const ctrlHeading = t('controllers.controller-num', [ctrl.controllerIndex]);
        const serialLabel = t('controllers.serial');
        // A pad on a stalling connection is about to die with it; tag the row
        // so it can't masquerade as a healthy duplicate.
        const staleTag = ctrl.connState === 'notResponding'
          ? ` <span class="device-state state-notResponding">${esc(deviceLinkStateLabel('notResponding'))}</span>`
          : '';
        return `
        <div class="ctrl-item">
          <div class="ctrl-row">
            <img class="ctrl-type-icon" src="img/ctrl-${esc(ctrlType)}.svg" alt="${esc(ctrlLabel)}" title="${esc(ctrlLabel)}">
            <div class="ctrl-info">
              <span class="ctrl-name"><span class="ctrl-dot ${ok ? 'ok' : 'err'}"></span>${esc(ctrlHeading)} · ${esc(ctrlLabel)}${staleTag}</span>
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
    showOffline();
    return;
  }
  checkBackendStatus();
}

async function disconnectConn(connId, btn) {
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
    // Don't restore() on success: the SSE `connections` tick removes the row,
    // and restoring would flash the glyph back first.
  } catch (e) {
    restore();
  }
}


function updatePinPanel(s) {
  const cur = document.getElementById('pin-current');
  const prev = document.getElementById('pin-previous');
  if (cur) cur.textContent = (s && s.currentPin) ? s.currentPin : '—';
  if (prev) prev.textContent = (s && s.previousPin) ? s.previousPin : '—';
  const hint = document.getElementById('pin-hint');
  if (!hint) return;
  if (s && s.state === 'paired') {
    hint.textContent = t('pin.hint.paired');
    return;
  }
  const secs = Math.max(0, parseInt(s && s.secondsRemaining, 10) || 0);
  const mm = Math.floor(secs / 60);
  const ss = (secs % 60).toString().padStart(2, '0');
  hint.textContent = t('pin.hint.active', [mm + ':' + ss]);
}

// One row per in-flight request; the operator compares the PIN against the
// device's screen and accepts/denies.
function updatePairRequests(list) {
  const section = document.getElementById('pair-request-section');
  const el = document.getElementById('pair-request-list');
  const reqs = Array.isArray(list) ? list : [];

  const badge = document.getElementById('pair-badge');
  if (badge) {
    badge.textContent = String(reqs.length);
    badge.style.display = reqs.length > 0 ? '' : 'none';
  }

  if (!section || !el) return;
  if (reqs.length === 0) {
    section.style.display = 'none';
    el.innerHTML = '';
    return;
  }
  section.style.display = '';

  const acceptLabel = t('pairreq.accept');
  const denyLabel = t('pairreq.deny');
  el.innerHTML = reqs.map(r => {
    const secs = Math.max(0, parseInt(r.secondsRemaining, 10) || 0);
    const mm = Math.floor(secs / 60);
    const ss = (secs % 60).toString().padStart(2, '0');
    return `
      <div class="device-item pair-request-item">
        <img class="device-glyph" src="img/icons/dish-scanning-animated.svg" alt="">
        <div class="device-info">
          <span class="device-name">${esc(r.deviceName || r.deviceId)}</span>
          <span class="device-meta">${esc(r.clientIP)} · ${esc(t('pairreq.expires', [mm + ':' + ss]))}</span>
        </div>
        <span class="pair-request-pin">${esc(r.pin || '')}</span>
        <div class="device-actions">
          <button class="btn btn-save" type="button" data-act="pair-accept" data-id="${esc(r.deviceId)}">${esc(acceptLabel)}</button>
          <button class="btn-icon btn-danger" type="button" data-act="pair-deny" data-id="${esc(r.deviceId)}" title="${esc(denyLabel)}"><img src="img/icons/close_x.svg" alt="${esc(denyLabel)}" class="emoji-icon"></button>
        </div>
      </div>`;
  }).join('');
}

function handlePairRequestClick(e) {
  const btn = e.target.closest('[data-act]');
  if (!btn) return;
  const id = btn.getAttribute('data-id') || '';
  if (btn.dataset.act === 'pair-deny') respondPairRequest(id, false, btn);
  else if (btn.dataset.act === 'pair-accept') respondPairRequest(id, true, btn);
}

async function respondPairRequest(deviceId, accept, btn) {
  const restore = setButtonLoading(btn, accept ? t('pairreq.accepting') : null);
  try {
    const res = await apiPost('/api/pair/respond', { deviceId, accept });
    // An expired/raced request returns HTTP 200 with ok:false; check both.
    if (!res.ok || (res.data && res.data.ok === false)) {
      showDashError(apiErrorText(res, accept ? t('pairreq.err.accept') : t('pairreq.err.deny')));
      restore();
      return;
    }
    hideDashError();
    // Leave the spinner; the SSE pairRequests tick drops this row within ~1s.
  } catch (e) {
    restore();
  }
}

// One row per paired device; its live session rides as a chip on the same row
// so one phone can't occupy multiple rows. Fed by the SSE devices +
// connections events; loadDevices() seeds the first paint.
let g_lastDevices = null;
let g_lastConnections = null;

function renderDeviceList() {
  const el = document.getElementById('device-list');
  if (!el) return;
  const devs = Array.isArray(g_lastDevices) ? g_lastDevices : [];
  if (devs.length === 0) {
    setHTML(el, '<p class="hint">' + esc(t('devices.empty')) + '</p>');
    return;
  }
  const conns = (g_lastConnections && Array.isArray(g_lastConnections.connections))
    ? g_lastConnections.connections : [];
  const removeLb = t('devices.remove');
  const disconnectLb = t('connections.disconnect');
  setHTML(el, devs.map(d => {
    const conn = conns.find(c => c.deviceId === d.id) || null;
    const stateKey = d.state || 'paired';
    const stateText = deviceLinkStateLabel(stateKey);
    const stateIcon = DEVICE_LINK_STATE_ICON[stateKey] || DEVICE_LINK_STATE_ICON.paired;
    let meta;
    if (conn) {
      const ctrlCount = conn.activeControllerCount || 0;
      const ctrlNoun = ctrlCount === 1 ? t('connections.controller.singular')
                                       : t('connections.controller.plural');
      meta = `${esc(conn.senderIP)} · ${ctrlCount} ${esc(ctrlNoun)}`;
    } else {
      meta = `${esc(d.lastIP)} · ${esc(d.pairedAt)}`;
    }
    // Kick only renders for a live session; unpair always.
    const kickBtn = conn
      ? `<button class="btn-icon" type="button" data-act="disconnect" data-conn-id="${esc(conn.connectionId)}" title="${esc(disconnectLb)}"><img src="img/icons/close_x.svg" alt="${esc(disconnectLb)}" class="emoji-icon"></button>`
      : '';
    return `
    <div class="device-item">
      <img class="device-glyph" src="img/icons/${esc(stateIcon)}" alt="">
      <div class="device-info">
        <span class="device-name">${esc(d.name)} <span class="device-state state-${esc(stateKey)}">${esc(stateText)}</span></span>
        <span class="device-meta">${meta}</span>
      </div>
      <div class="device-actions">
        ${kickBtn}
        <button class="btn-icon btn-danger" type="button" data-act="remove-device" data-id="${esc(d.id)}" title="${esc(removeLb)}"><img src="img/icons/close_x.svg" alt="${esc(removeLb)}" class="emoji-icon"></button>
      </div>
    </div>`;
  }).join(''));
}

// First-paint seed before SSE connects, and refresh after unpair.
async function loadDevices() {
  try {
    const r = await fetch('/api/devices');
    if (!r.ok) return;
    g_lastDevices = await r.json();
    renderDeviceList();
  } catch (e) { /* ignore */ }
}

// Delegated handler keeps ids out of inline onclick= JS-string contexts;
// esc() is an HTML escaper, not a JS-string escaper.
function handleDeviceListClick(e) {
  const btn = e.target.closest('[data-act]');
  if (!btn) return;
  if (btn.dataset.act === 'remove-device') {
    removeDevice(btn.getAttribute('data-id') || '', btn);
  } else if (btn.dataset.act === 'disconnect') {
    disconnectConn(btn.getAttribute('data-conn-id') || '', btn);
  }
}

async function removeDevice(id, btn) {
  // DELETE /api/devices/<id> unpairs and closes any live session.
  const restore = setButtonLoading(btn);
  try {
    const res = await api('/api/devices/' + encodeURIComponent(id), { method: 'DELETE' });
    if (!res.ok) {
      showDashError(apiErrorText(res, t('devices.err.remove')));
      restore();
      return;
    }
    hideDashError();
    // Don't restore(); loadDevices() rebuilds the list and removes this button.
    loadDevices();
  } catch (e) {
    restore();
  }
}

let backendGuideOpen = false;

function updateBackendPanel(backend, backendActive) {
  const alert = document.getElementById('backend-alert');
  if (!alert) return;

  if (!backend || !backend.supported || backend.available) {
    alert.classList.remove('show');
    const guide = document.getElementById('backend-guide');
    if (guide) guide.style.display = 'none';
    backendGuideOpen = false;
    return;
  }

  alert.classList.add('show');
  const copy  = backendCopy(backend.id);
  const err   = (copy.errors && copy.errors[backend.errorCode]) || null;
  const label = document.getElementById('backend-label');
  if (label) label.textContent = err ? err.title : t('backend.unavailable');
  populateBackendGuide(err);
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


