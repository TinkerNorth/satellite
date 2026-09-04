let settingsSavedConfig = {
  udpPort: 9876, autoStart: false, discoveryBroadcast: true, controllerAudio: true,
  controllerAudioMic: true, controllerAudioSpeaker: true,
  controllerAudioKeepDefaultDevice: true,
};

// The audio sub-switches, which all follow one pattern: DOM id, saved-config
// key and POST key are the same string.
const SETTINGS_AUDIO_KEYS = [
  'controllerAudioMic', 'controllerAudioSpeaker', 'controllerAudioKeepDefaultDevice',
];

// Same DOM-id/config-key/POST-key convention as the audio switches, but NOT
// gated by the controller-audio master switch, and with the opposite default:
// an absent key reads as OFF. These are consent switches, so a server that
// predates them, or one that never answered, must not be shown as opted in.
const SETTINGS_OPTIN_KEYS = ['crashReporting'];

function settingsOptInEl(key) { return document.getElementById('settings-' + key); }

function settingsAudioEl(key) { return document.getElementById('settings-' + key); }

function settingsCheckDirty() {
  const curPort = parseInt(document.getElementById('settings-udpPort').value, 10);
  const curAuto = document.getElementById('settings-autoStart').checked;
  const curBroadcast = document.getElementById('settings-discoveryBroadcast').checked;
  const curAudio = document.getElementById('settings-controllerAudio').checked;
  const audioDirty = SETTINGS_AUDIO_KEYS.some((k) => {
    const el = settingsAudioEl(k);
    return el && el.checked !== settingsSavedConfig[k];
  });
  const optInDirty = SETTINGS_OPTIN_KEYS.some((k) => {
    const el = settingsOptInEl(k);
    return el && el.checked !== settingsSavedConfig[k];
  });
  const dirty =
    curPort !== settingsSavedConfig.udpPort ||
    curAuto !== settingsSavedConfig.autoStart ||
    curBroadcast !== settingsSavedConfig.discoveryBroadcast ||
    curAudio !== settingsSavedConfig.controllerAudio ||
    audioDirty ||
    optInDirty;
  document.getElementById('settings-btnSave').disabled = !dirty;
  document.getElementById('settings-btnUndo').disabled = !dirty;
  // Editing clears stale validation/save messages so a prior error doesn't
  // linger over freshly-changed values.
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
  document.getElementById('settings-controllerAudio').checked =
    settingsSavedConfig.controllerAudio;
  SETTINGS_AUDIO_KEYS.forEach((k) => {
    const el = settingsAudioEl(k);
    if (el) el.checked = settingsSavedConfig[k];
  });
  SETTINGS_OPTIN_KEYS.forEach((k) => {
    const el = settingsOptInEl(k);
    if (el) el.checked = settingsSavedConfig[k];
  });
  settingsSetPortError('');
  settingsSetSaveStatus('', false);
  settingsCheckDirty();
}

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
  const audio = document.getElementById('settings-controllerAudio').checked;

  // Client-side range check: the server silently clamps a port outside
  // 1024-65535 and still returns ok:true, so without this the user gets no
  // feedback that their value was rejected.
  settingsSetSaveStatus('', false);
  if (!Number.isInteger(port) || port < 1024 || port > 65535) {
    settingsSetPortError(t('settings.udp-port.error'));
    return;
  }
  settingsSetPortError('');

  const payload = {
    udpPort: port,
    autoStart: auto,
    discoveryBroadcastEnabled: broadcast,
    controllerAudio: audio,
  };
  SETTINGS_AUDIO_KEYS.forEach((k) => {
    const el = settingsAudioEl(k);
    if (el) payload[k] = el.checked;
  });
  SETTINGS_OPTIN_KEYS.forEach((k) => {
    const el = settingsOptInEl(k);
    if (el) payload[k] = el.checked;
  });
  const res = await apiPost('/api/config', payload);

  // Only commit saved-state when the server confirmed the write, else a
  // failed POST would clear the dirty flag and look like a success.
  if (!res.ok) {
    settingsSetSaveStatus(
      res.status === 0
        ? t('settings.save.unreachable')
        : t('settings.save.failed', [(res.data && res.data.error) || t('settings.save.server-error')]),
      false);
    return;
  }

  // The server owns the accepted range, so re-seed the field from its echoed
  // udpPort rather than assuming our request stuck verbatim.
  const effectivePort = (res.data && typeof res.data.udpPort === 'number')
    ? res.data.udpPort : port;
  settingsSavedConfig.udpPort = effectivePort;
  settingsSavedConfig.autoStart = auto;
  settingsSavedConfig.discoveryBroadcast = broadcast;
  settingsSavedConfig.controllerAudio = audio;
  SETTINGS_AUDIO_KEYS.forEach((k) => {
    const el = settingsAudioEl(k);
    if (el) settingsSavedConfig[k] = el.checked;
  });
  SETTINGS_OPTIN_KEYS.forEach((k) => {
    const el = settingsOptInEl(k);
    if (el) settingsSavedConfig[k] = el.checked;
  });
  document.getElementById('settings-udpPort').value = effectivePort;
  settingsCheckDirty();

  // If the server reports it rejected the port, say so rather than claiming
  // success with a value that didn't take.
  if (res.data && res.data.udpPortRejected) {
    settingsSetPortError(t('settings.udp-port.rejected', [effectivePort]));
    return;
  }
  settingsSetSaveStatus(t('settings.save.ok'), true);
  initNetworkSettings();
}

// Inactive means the responder couldn't bind 5353, so discovery falls back to
// the legacy beacon; flag it with the warning colour.
function settingsRenderMdnsStatus(active) {
  const el = document.getElementById('settings-mdns-status');
  if (!el) return;
  el.textContent = active ? t('settings.mdns.active') : t('settings.mdns.inactive');
  el.style.color = active ? 'var(--success)' : 'var(--warning)';
}

async function initSettings() {
  // Re-seed the updates form from the next snapshot in case prefs changed in
  // another tab while we were on /dashboard.
  if (typeof updatesResetForm === 'function') updatesResetForm();

  try {
    const r = await fetch('/api/status');
    const d = await r.json();
    settingsSavedConfig.udpPort = d.udpPort;
    settingsSavedConfig.autoStart = d.autoStart;
    // Absent key (pre-1.6 server) defaults to on, matching the config default.
    settingsSavedConfig.discoveryBroadcast = d.discoveryBroadcastEnabled !== false;
    // Same rule for a server that predates controller audio: absent is on.
    settingsSavedConfig.controllerAudio = d.controllerAudio !== false;
    SETTINGS_AUDIO_KEYS.forEach((k) => { settingsSavedConfig[k] = d[k] !== false; });
    // `=== true`, not `!== false`: consent defaults to off when the server
    // says nothing, which is the reverse of every switch above.
    SETTINGS_OPTIN_KEYS.forEach((k) => { settingsSavedConfig[k] = d[k] === true; });
    document.getElementById('settings-udpPort').value = d.udpPort;
    document.getElementById('settings-autoStart').checked = d.autoStart;
    document.getElementById('settings-discoveryBroadcast').checked =
      settingsSavedConfig.discoveryBroadcast;
    document.getElementById('settings-controllerAudio').checked =
      settingsSavedConfig.controllerAudio;
    SETTINGS_AUDIO_KEYS.forEach((k) => {
      const el = settingsAudioEl(k);
      if (el) el.checked = settingsSavedConfig[k];
    });
    SETTINGS_OPTIN_KEYS.forEach((k) => {
      const el = settingsOptInEl(k);
      if (el) el.checked = settingsSavedConfig[k];
    });
    settingsRenderCrashReportingHint(d.crashReportingActive === true,
                                     settingsSavedConfig.crashReporting === true);
    settingsRenderMdnsStatus(d.mdnsResponderActive === true);
  } catch (e) { /* ignore */ }

  settingsCheckDirty();

  const udp = document.getElementById('settings-udpPort');
  const auto = document.getElementById('settings-autoStart');
  const broadcast = document.getElementById('settings-discoveryBroadcast');
  const audio = document.getElementById('settings-controllerAudio');
  if (udp) udp.addEventListener('input', settingsCheckDirty);
  if (auto) auto.addEventListener('change', settingsCheckDirty);
  if (broadcast) broadcast.addEventListener('change', settingsCheckDirty);
  if (audio) audio.addEventListener('change', settingsCheckDirty);
  SETTINGS_AUDIO_KEYS.forEach((k) => {
    const el = settingsAudioEl(k);
    if (el) el.addEventListener('change', settingsCheckDirty);
  });
  SETTINGS_OPTIN_KEYS.forEach((k) => {
    const el = settingsOptInEl(k);
    if (el) el.addEventListener('change', settingsCheckDirty);
  });

  // "Start with Windows" reads odd on Mac/Linux, so adapt the label per OS.
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
      const label = (f.name || '') + ': ' + (f.ip || '') + tag;
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

// A build with no DSN compiled in cannot transmit no matter what the switch
// says, and the UI must not imply otherwise. This is the honest-label rule:
// say "on, and sending" only when the server confirms it actually armed.
function settingsRenderCrashReportingHint(active, wanted) {
  const el = document.getElementById('settings-crashReporting-status');
  if (!el) return;
  if (!wanted) { el.textContent = t('settings.crash.status.off'); return; }
  el.textContent = active ? t('settings.crash.status.on')
                          : t('settings.crash.status.unavailable');
}
