// ── settings.js — Server configuration page ─────────────────────────────────

let settingsSavedConfig = { udpPort: 9876, autoStart: false, discoveryBroadcast: true };

function settingsCheckDirty() {
  const curPort = parseInt(document.getElementById('settings-udpPort').value);
  const curAuto = document.getElementById('settings-autoStart').checked;
  const curBroadcast = document.getElementById('settings-discoveryBroadcast').checked;
  const dirty =
    curPort !== settingsSavedConfig.udpPort ||
    curAuto !== settingsSavedConfig.autoStart ||
    curBroadcast !== settingsSavedConfig.discoveryBroadcast;
  document.getElementById('settings-btnSave').disabled = !dirty;
  document.getElementById('settings-btnUndo').disabled = !dirty;
}

function settingsUndo() {
  document.getElementById('settings-udpPort').value = settingsSavedConfig.udpPort;
  document.getElementById('settings-autoStart').checked = settingsSavedConfig.autoStart;
  document.getElementById('settings-discoveryBroadcast').checked =
    settingsSavedConfig.discoveryBroadcast;
  settingsCheckDirty();
}

async function settingsSave() {
  const port = parseInt(document.getElementById('settings-udpPort').value);
  const auto = document.getElementById('settings-autoStart').checked;
  const broadcast = document.getElementById('settings-discoveryBroadcast').checked;
  await apiPost('/api/config', {
    udpPort: port,
    autoStart: auto,
    discoveryBroadcastEnabled: broadcast,
  });
  settingsSavedConfig.udpPort = port;
  settingsSavedConfig.autoStart = auto;
  settingsSavedConfig.discoveryBroadcast = broadcast;
  settingsCheckDirty();
}

// Render the read-only mDNS responder state. Active is the healthy path;
// Inactive means the responder couldn't bind 5353 — discovery then relies on
// the legacy broadcast beacon, so it's flagged with the warning colour.
function settingsRenderMdnsStatus(active) {
  const el = document.getElementById('settings-mdns-status');
  if (!el) return;
  el.textContent = active ? 'Active' : 'Inactive';
  el.style.color = active ? 'var(--success)' : 'var(--warning)';
}

async function initSettings() {
  // Force the updates form to re-seed from the next snapshot — handles the
  // case where prefs were changed in another tab while we were on /dashboard.
  if (typeof updatesResetForm === 'function') updatesResetForm();

  // Load current config from server
  try {
    const r = await fetch('/api/status');
    if (r.status === 401) { navigate('/login'); return; }
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
          case 'macos':           lbl.textContent = 'Start at login'; break;
          case 'linux-appimage':
          case 'linux-deb':
          case 'linux-portable':  lbl.textContent = 'Start at login'; break;
          default:                lbl.textContent = 'Start with Windows';
        }
      }
    }
  } catch (e) { /* ignore */ }

  // Pull update state and prefs. updates.js renders both the banner + form.
  if (typeof updatesFetch === 'function') updatesFetch();
}
