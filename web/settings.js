// ── settings.js — Server configuration page ─────────────────────────────────

let settingsSavedConfig = { udpPort: 9876, autoStart: false, discoveryBroadcast: true };

function settingsCheckDirty() {
  const curPort = parseInt(document.getElementById('settings-udpPort').value, 10);
  const curAuto = document.getElementById('settings-autoStart').checked;
  const curBroadcast = document.getElementById('settings-discoveryBroadcast').checked;
  const dirty =
    curPort !== settingsSavedConfig.udpPort ||
    curAuto !== settingsSavedConfig.autoStart ||
    curBroadcast !== settingsSavedConfig.discoveryBroadcast;
  document.getElementById('settings-btnSave').disabled = !dirty;
  document.getElementById('settings-btnUndo').disabled = !dirty;
  // Editing the form clears any stale validation / save-result message so a
  // previous error doesn't linger over freshly-changed values.
  if (dirty) {
    settingsSetPortError('');
    settingsSetSaveStatus('', false);
  }
}

function settingsUndo() {
  document.getElementById('settings-udpPort').value = settingsSavedConfig.udpPort;
  document.getElementById('settings-autoStart').checked = settingsSavedConfig.autoStart;
  document.getElementById('settings-discoveryBroadcast').checked =
    settingsSavedConfig.discoveryBroadcast;
  settingsSetPortError('');
  settingsSetSaveStatus('', false);
  settingsCheckDirty();
}

// Clear or set the inline status lines under the form.
function settingsSetPortError(msg) {
  const el = document.getElementById('settings-udpPort-error');
  if (el) el.textContent = msg || '';
}
function settingsSetSaveStatus(msg, ok) {
  const el = document.getElementById('settings-save-status');
  if (!el) return;
  el.textContent = msg || '';
  el.classList.toggle('ok', !!ok);
}

async function settingsSave() {
  const port = parseInt(document.getElementById('settings-udpPort').value, 10);
  const auto = document.getElementById('settings-autoStart').checked;
  const broadcast = document.getElementById('settings-discoveryBroadcast').checked;

  // Client-side range check. The server silently clamps/ignores a port
  // outside 1024–65535 and still returns {"ok":true}, so without this the
  // user gets no feedback that their value was rejected. Mirrors the
  // <input min/max> and the server's own accepted range.
  settingsSetSaveStatus('', false);
  if (!Number.isInteger(port) || port < 1024 || port > 65535) {
    settingsSetPortError(t('settings.udp-port.error'));
    return;
  }
  settingsSetPortError('');

  const res = await apiPost('/api/config', {
    udpPort: port,
    autoStart: auto,
    discoveryBroadcastEnabled: broadcast,
  });

  // Only commit saved-state when the server confirmed the write. A failed
  // POST previously still cleared the dirty flag, making a failure look
  // like a success.
  if (!res.ok) {
    settingsSetSaveStatus(
      res.status === 0
        ? t('settings.save.unreachable')
        : t('settings.save.failed', [(res.data && res.data.error) || t('settings.save.server-error')]),
      false);
    return;
  }

  // Reflect the server's effective value — the server owns the accepted
  // range, so re-seed the field from its response rather than assuming our
  // request stuck verbatim. `udpPort` is echoed by POST /api/config.
  const effectivePort = (res.data && typeof res.data.udpPort === 'number')
    ? res.data.udpPort : port;
  settingsSavedConfig.udpPort = effectivePort;
  settingsSavedConfig.autoStart = auto;
  settingsSavedConfig.discoveryBroadcast = broadcast;
  document.getElementById('settings-udpPort').value = effectivePort;
  settingsCheckDirty();

  // The client range-checks before POST, so this should not fire from the
  // web UI — but if the server reports it rejected the port, say so rather
  // than claiming success with a value that didn't take.
  if (res.data && res.data.udpPortRejected) {
    settingsSetPortError(t('settings.udp-port.rejected', [effectivePort]));
    return;
  }
  settingsSetSaveStatus(t('settings.save.ok'), true);
  initNetworkSettings();
}

// Render the read-only mDNS responder state. Active is the healthy path;
// Inactive means the responder couldn't bind 5353 — discovery then relies on
// the legacy broadcast beacon, so it's flagged with the warning colour.
function settingsRenderMdnsStatus(active) {
  const el = document.getElementById('settings-mdns-status');
  if (!el) return;
  el.textContent = active ? t('settings.mdns.active') : t('settings.mdns.inactive');
  el.style.color = active ? 'var(--success)' : 'var(--warning)';
}

async function initSettings() {
  // Force the updates form to re-seed from the next snapshot — handles the
  // case where prefs were changed in another tab while we were on /dashboard.
  if (typeof updatesResetForm === 'function') updatesResetForm();

  // Load current config from server
  try {
    const r = await fetch('/api/status');
    const d = await r.json();
    settingsSavedConfig.udpPort = d.udpPort;
    settingsSavedConfig.autoStart = d.autoStart;
    // Absent key (pre-1.6 server) → treat as on, matching the config default.
    settingsSavedConfig.discoveryBroadcast = d.discoveryBroadcastEnabled !== false;
    document.getElementById('settings-udpPort').value = d.udpPort;
    document.getElementById('settings-autoStart').checked = d.autoStart;
    document.getElementById('settings-discoveryBroadcast').checked =
      settingsSavedConfig.discoveryBroadcast;
    settingsRenderMdnsStatus(d.mdnsResponderActive === true);
  } catch (e) { /* ignore */ }

  settingsCheckDirty();

  const udp = document.getElementById('settings-udpPort');
  const auto = document.getElementById('settings-autoStart');
  const broadcast = document.getElementById('settings-discoveryBroadcast');
  if (udp) udp.addEventListener('input', settingsCheckDirty);
  if (auto) auto.addEventListener('change', settingsCheckDirty);
  if (broadcast) broadcast.addEventListener('change', settingsCheckDirty);

  // Adapt the autostart label per OS — "Start with Windows" reads odd on Mac/Linux.
  try {
    const vr = await fetch('/api/version');
    if (vr.ok) {
      const v = await vr.json();
      const lbl = document.getElementById('settings-autoStart-label');
      if (lbl) {
        switch (v.platformId) {
          case 'macos':           lbl.textContent = t('settings.autostart.login'); break;
          case 'linux-appimage':
          case 'linux-deb':
          case 'linux-portable':  lbl.textContent = t('settings.autostart.login'); break;
          default:                lbl.textContent = t('settings.autostart.windows');
        }
      }
    }
  } catch (e) { /* ignore */ }

  // Pull update state and prefs. updates.js renders both the banner + form.
  if (typeof updatesFetch === 'function') updatesFetch();

  initNetworkSettings();
}

async function initNetworkSettings() {
  const d = await getNetInfo();
  if (!d) return;
  renderNetInfoPanel('settings-netinfo', d);
  settingsRenderInterfaceOptions(d);
  settingsRenderNetWarning(d);
}

function settingsRenderInterfaceOptions(d) {
  const sel = document.getElementById('settings-interface');
  if (!sel) return;
  let html = '<option value="">' + esc(t('netinfo.interface.auto')) + '</option>';
  if (Array.isArray(d.interfaces)) {
    for (const f of d.interfaces) {
      const tag = f.physical ? '' : ' ' + t('netinfo.virtual');
      const label = (f.name || '') + ' — ' + (f.ip || '') + tag;
      html += '<option value="' + esc(f.name) + '">' + esc(label) + '</option>';
    }
  }
  sel.innerHTML = html;
  sel.value = d.selected || '';
  sel.onchange = settingsInterfaceChange;
}

function settingsRenderNetWarning(d) {
  const el = document.getElementById('settings-net-warning');
  if (!el) return;
  if (d.category === 'public' && !d.allowPublic) {
    el.innerHTML = '<span>' + esc(t('netinfo.public.warn')) + ' </span>' +
      '<button class="btn btn-save" id="settings-allow-public" type="button">' +
      esc(t('netinfo.public.allow')) + '</button> ' +
      '<span id="settings-allow-public-status"></span>';
    el.classList.add('show');
    const btn = document.getElementById('settings-allow-public');
    if (btn) btn.onclick = settingsAllowPublic;
    return;
  }
  const fwState = d.firewall && d.firewall.supported ? d.firewall.state : '';
  if (fwState === 'missing' || fwState === 'wrong-profile') {
    el.innerHTML = '<span>' + esc(t('netinfo.firewall.warn')) + '</span>';
    el.classList.add('show');
    return;
  }
  el.classList.remove('show');
  el.innerHTML = '';
}

async function settingsAllowPublic() {
  const status = document.getElementById('settings-allow-public-status');
  if (status) status.textContent = t('netinfo.public.allowing');
  const res = await apiPost('/api/network/allow-public', {});
  if (res.ok && res.data && res.data.ok) {
    if (status) status.textContent = t('netinfo.public.allowed');
    initNetworkSettings();
  } else {
    if (status) status.textContent = t('netinfo.public.cancelled');
  }
}

async function settingsInterfaceChange() {
  const sel = document.getElementById('settings-interface');
  if (!sel) return;
  const res = await apiPost('/api/config', { networkInterface: sel.value });
  const note = document.getElementById('settings-interface-note');
  if (note) note.textContent = res.ok ? t('netinfo.restart-note') : t('settings.save.unreachable');
  initNetworkSettings();
}
