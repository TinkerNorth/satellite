// ── settings.js — Server configuration page ─────────────────────────────────

let settingsSavedConfig = { udpPort: 9876, autoStart: false };

function settingsCheckDirty() {
  const curPort = parseInt(document.getElementById('settings-udpPort').value);
  const curAuto = document.getElementById('settings-autoStart').checked;
  const dirty = curPort !== settingsSavedConfig.udpPort || curAuto !== settingsSavedConfig.autoStart;
  document.getElementById('settings-btnSave').disabled = !dirty;
  document.getElementById('settings-btnUndo').disabled = !dirty;
}

function settingsUndo() {
  document.getElementById('settings-udpPort').value = settingsSavedConfig.udpPort;
  document.getElementById('settings-autoStart').checked = settingsSavedConfig.autoStart;
  settingsCheckDirty();
}

async function settingsSave() {
  const port = parseInt(document.getElementById('settings-udpPort').value);
  const auto = document.getElementById('settings-autoStart').checked;
  await apiPost('/api/config', { udpPort: port, autoStart: auto });
  settingsSavedConfig.udpPort = port;
  settingsSavedConfig.autoStart = auto;
  settingsCheckDirty();
}

async function initSettings() {
  // Load current config from server
  try {
    const r = await fetch('/api/status');
    if (r.status === 401) { navigate('/login'); return; }
    const d = await r.json();
    settingsSavedConfig.udpPort = d.udpPort;
    settingsSavedConfig.autoStart = d.autoStart;
    document.getElementById('settings-udpPort').value = d.udpPort;
    document.getElementById('settings-autoStart').checked = d.autoStart;
  } catch (e) { /* ignore */ }

  settingsCheckDirty();

  const udp = document.getElementById('settings-udpPort');
  const auto = document.getElementById('settings-autoStart');
  if (udp) udp.addEventListener('input', settingsCheckDirty);
  if (auto) auto.addEventListener('change', settingsCheckDirty);
}
