// ── updates.js — OTA update UI flows ──────────────────────────────────────
//
// Single source of truth for the "update available / downloading / ready"
// experience. Listens to the SSE "update" event the server broadcasts each
// tick, renders the banner state across both dashboard + settings, owns
// the restart-confirmation modal.
//
// The banner template (#tpl-update-banner) is cloned into two slots:
//   #dashboard-update-slot  — top of /dashboard, banner-style alert
//   #settings-update-slot   — bottom of the Updates section in /settings
// Each slot keeps its own banner element. State is held centrally in
// updatesState and pushed to both renderers on every change.

const UPDATE_STATE_IDLE        = 'idle';
const UPDATE_STATE_CHECKING    = 'checking';
const UPDATE_STATE_UP_TO_DATE  = 'up-to-date';
const UPDATE_STATE_AVAILABLE   = 'update-available';
const UPDATE_STATE_DOWNLOADING = 'downloading';
const UPDATE_STATE_VERIFYING   = 'verifying';
const UPDATE_STATE_DOWNLOADED  = 'downloaded';
const UPDATE_STATE_INSTALLING  = 'installing';
const UPDATE_STATE_ERROR       = 'error';

let updatesState = null; // last snapshot pushed by SSE / fetch

// ── Snapshot fetch (used on page mount; SSE takes over thereafter) ────────
async function updatesFetch() {
  try {
    const r = await fetch('/api/updates/status');
    if (!r.ok) return null;
    const d = await r.json();
    updatesState = d;
    updatesRenderAll();
    return d;
  } catch (e) {
    return null;
  }
}

// ── SSE event handler ─────────────────────────────────────────────────────
// dashboard.js (which owns the EventSource) calls this on every "update"
// frame. We just store + re-render.
function updatesHandleSSE(snapshot) {
  updatesState = snapshot;
  updatesRenderAll();
}

// ── Banner mount/render ───────────────────────────────────────────────────
function updatesMount(slotId) {
  const slot = document.getElementById(slotId);
  if (!slot) return null;
  if (!slot.firstElementChild) {
    const tpl = document.getElementById('tpl-update-banner');
    if (!tpl) return null;
    const node = tpl.content.firstElementChild.cloneNode(true);
    // De-dupe IDs by suffixing with the slot id so dashboard + settings
    // banners don't collide on document.getElementById.
    const suffix = '-' + slotId;
    ['update-banner', 'update-banner-title', 'update-banner-detail',
     'update-banner-progress', 'update-banner-progress-fill',
     'update-banner-progress-text', 'update-banner-actions'].forEach(id => {
      const el = node.querySelector('#' + id);
      if (el) el.id = id + suffix;
    });
    slot.appendChild(node);
  }
  return slot.firstElementChild;
}

function updatesRenderAll() {
  updatesRender('dashboard-update-slot');
  updatesRender('settings-update-slot');
  updatesRenderSettingsFields();
}

function updatesRender(slotId) {
  const banner = updatesMount(slotId);
  if (!banner || !updatesState) return;
  const s = updatesState;
  const showWhen = new Set([
    UPDATE_STATE_AVAILABLE, UPDATE_STATE_DOWNLOADING,
    UPDATE_STATE_VERIFYING, UPDATE_STATE_DOWNLOADED,
    UPDATE_STATE_INSTALLING, UPDATE_STATE_ERROR,
  ]);
  // On the settings page, also show the "checking…" and "up-to-date" states
  // so the user sees feedback from clicking "Check for Updates".
  const onSettings = slotId === 'settings-update-slot';
  // A failed background check (network down, GitHub unreachable, rate-limit)
  // is noise on the dashboard — the user didn't initiate it and can't act on
  // it from there. Keep it on settings where it belongs next to the "Check
  // for Updates" button.
  const isCheckError = s.state === UPDATE_STATE_ERROR &&
                       s.failedPhase === UPDATE_STATE_CHECKING;
  const visible = (showWhen.has(s.state) && !(isCheckError && !onSettings)) ||
                  (onSettings && (s.state === UPDATE_STATE_CHECKING ||
                                  s.state === UPDATE_STATE_UP_TO_DATE));
  banner.style.display = visible ? 'flex' : 'none';
  if (!visible) return;

  const suffix = '-' + slotId;
  const title  = document.getElementById('update-banner-title' + suffix);
  const detail = document.getElementById('update-banner-detail' + suffix);
  const prog   = document.getElementById('update-banner-progress' + suffix);
  const fill   = document.getElementById('update-banner-progress-fill' + suffix);
  const pct    = document.getElementById('update-banner-progress-text' + suffix);
  const acts   = document.getElementById('update-banner-actions' + suffix);
  if (!title || !detail || !acts) return;

  banner.classList.remove('update-banner-error');
  prog.style.display = 'none';
  acts.innerHTML = '';

  switch (s.state) {
    case UPDATE_STATE_CHECKING:
      title.textContent = t('updates.state.checking.title');
      detail.textContent = '';
      break;
    case UPDATE_STATE_UP_TO_DATE:
      title.textContent = t('updates.state.uptodate.title');
      detail.textContent = t('updates.state.uptodate.detail', [s.currentVersion]);
      break;
    case UPDATE_STATE_AVAILABLE: {
      title.textContent = t('updates.state.available.title', [s.info.version]);
      const size = s.info.assetSize ? ' · ' + formatBytes(s.info.assetSize) : '';
      detail.textContent = s.info.assetName + size;
      if (s.info.installMethod === 'manual') {
        acts.appendChild(makeBtn('btn-undo', t('updates.btn.view-release'), () => openExternal(s.info.htmlUrl)));
        if (s.info.manualInstruction) {
          const code = document.createElement('pre');
          code.className = 'update-manual-cmd';
          code.textContent = s.info.manualInstruction;
          detail.appendChild(document.createElement('br'));
          detail.appendChild(code);
        }
      } else {
        acts.appendChild(makeBtn('btn-start', t('updates.btn.download'),       () => updatesDownload()));
        acts.appendChild(makeBtn('btn-undo',  t('updates.btn.view-notes'),     () => openExternal(s.info.htmlUrl)));
        acts.appendChild(makeBtn('btn-undo',  t('updates.btn.remind-later'),   () => updatesDismiss()));
        acts.appendChild(makeBtn('btn-undo',  t('updates.btn.skip-version'),   () => updatesSkip(s.info.version)));
      }
      break;
    }
    case UPDATE_STATE_DOWNLOADING: {
      title.textContent = t('updates.state.downloading.title', [s.info.version]);
      detail.textContent = t('updates.state.downloading.detail',
                             [formatBytes(s.bytesDownloaded),
                              s.totalBytes ? formatBytes(s.totalBytes) : t('updates.state.downloading.detail-unknown')]);
      prog.style.display = 'block';
      const p = s.totalBytes ? Math.min(100, Math.round(100 * s.bytesDownloaded / s.totalBytes)) : 0;
      fill.style.width = p + '%';
      pct.textContent = p + '%';
      acts.appendChild(makeBtn('btn-undo', t('updates.btn.cancel'), () => updatesCancel()));
      break;
    }
    case UPDATE_STATE_VERIFYING:
      title.textContent = t('updates.state.verifying.title');
      detail.textContent = t('updates.state.verifying.detail', [s.info.assetName]);
      break;
    case UPDATE_STATE_DOWNLOADED:
      title.textContent = t('updates.state.downloaded.title', [s.info.version]);
      detail.textContent = t('updates.state.downloaded.detail');
      acts.appendChild(makeBtn('btn-start', t('updates.btn.restart-install'), () => updatesPromptRestart()));
      acts.appendChild(makeBtn('btn-undo',  t('updates.btn.install-later'),   () => updatesDismiss()));
      break;
    case UPDATE_STATE_INSTALLING:
      title.textContent = t('updates.state.installing.title', [s.info.version]);
      detail.textContent = t('updates.state.installing.detail');
      prog.style.display = 'block';
      fill.style.width = '100%';
      pct.textContent = '';
      break;
    case UPDATE_STATE_ERROR:
      banner.classList.add('update-banner-error');
      title.textContent = updateErrorTitle(s.failedPhase);
      detail.textContent = s.message || t('updates.error.unknown');
      acts.appendChild(makeBtn('btn-undo', t('updates.btn.try-again'), () => updatesCheck()));
      break;
    default:
      banner.style.display = 'none';
  }
}

// ── Settings-page field bindings ──────────────────────────────────────────
// Mirror /api/updates/status into the read-only info rows on every tick.
// Form controls (channel + auto* toggles) are only seeded once per page
// mount — otherwise a 1 Hz SSE re-render would overwrite the user's
// in-progress edits the moment they blur a field. updatesSeedFormOnce
// does that one-shot seeding; updatesRenderSettingsFields just refreshes
// the timestamps and resets disabled-flags between dependent toggles.
let savedPrefs = null;
let formSeeded = false;

function updatesSeedFormOnce() {
  if (formSeeded || !updatesState) return;
  const s = updatesState;
  const ch = document.getElementById('settings-channel');
  const ac = document.getElementById('settings-autoCheck');
  const ad = document.getElementById('settings-autoDownload');
  const ai = document.getElementById('settings-autoInstall');
  if (!ch || !ac || !ad || !ai) return; // settings page not mounted yet
  ch.value = s.channel || 'stable';
  ac.checked = !!s.autoCheck;
  ad.checked = !!s.autoDownload;
  ai.checked = !!s.autoInstall;
  ad.disabled = !ac.checked;
  ai.disabled = !ad.checked || ad.disabled;
  savedPrefs = {
    channel: s.channel || 'stable',
    autoCheck: !!s.autoCheck,
    autoDownload: !!s.autoDownload,
    autoInstall: !!s.autoInstall,
  };
  formSeeded = true;
  updatesCheckPrefsDirty();
}

function updatesRenderSettingsFields() {
  if (!updatesState) return;
  const s = updatesState;
  setText('settings-version', 'v' + s.currentVersion);
  setText('settings-platform', formatPlatformId(s.platformId));
  setText('settings-last-check', s.lastCheckEpoch ? formatRelativeEpoch(s.lastCheckEpoch) : t('settings.updates.never'));
  updatesSeedFormOnce();
}

function updatesCheckPrefsDirty() {
  if (!savedPrefs) return;
  const cur = {
    channel: document.getElementById('settings-channel')?.value,
    autoCheck: !!document.getElementById('settings-autoCheck')?.checked,
    autoDownload: !!document.getElementById('settings-autoDownload')?.checked,
    autoInstall: !!document.getElementById('settings-autoInstall')?.checked,
  };
  const dirty = cur.channel !== savedPrefs.channel ||
                cur.autoCheck !== savedPrefs.autoCheck ||
                cur.autoDownload !== savedPrefs.autoDownload ||
                cur.autoInstall !== savedPrefs.autoInstall;
  const btn = document.getElementById('settings-btnUpdatePrefs');
  if (btn) btn.disabled = !dirty;
}

// ── Imperative actions (called from buttons + tray-equivalents) ───────────
async function updatesCheck() {
  await apiPost('/api/updates/check');
  // SSE will deliver the new snapshot; render an optimistic "Checking…" right away.
  if (updatesState) {
    updatesState = { ...updatesState, state: UPDATE_STATE_CHECKING };
    updatesRenderAll();
  }
}

async function updatesDownload() {
  await apiPost('/api/updates/download');
}

async function updatesInstall() {
  await apiPost('/api/updates/install');
}

async function updatesCancel() {
  await apiPost('/api/updates/cancel');
}

async function updatesDismiss() {
  await apiPost('/api/updates/dismiss');
}

async function updatesSkip(version) {
  await apiPost('/api/updates/skip', { version });
}

async function updatesSavePrefs() {
  const body = {
    channel: document.getElementById('settings-channel').value,
    autoCheck: document.getElementById('settings-autoCheck').checked,
    autoDownload: document.getElementById('settings-autoDownload').checked,
    autoInstall: document.getElementById('settings-autoInstall').checked,
  };
  await apiPost('/api/updates/preferences', body);
  savedPrefs = { ...body };
  updatesCheckPrefsDirty();
}

// Called when the user navigates away from /settings so the next visit
// reseeds from the latest server snapshot.
function updatesResetForm() {
  formSeeded = false;
  savedPrefs = null;
}

// ── Restart-confirmation modal ────────────────────────────────────────────
function updatesPromptRestart() {
  if (!updatesState || !updatesState.info) return;
  // Replace the modal body in-place so the version is interpolated by the
  // i18n catalog (rather than spliced into hard-coded English markup).
  const body = document.getElementById('restart-modal-body');
  if (body) body.textContent = t('modal.restart.body', ['v' + updatesState.info.version]);
  const verEl = document.getElementById('restart-modal-version');
  if (verEl) verEl.textContent = 'v' + updatesState.info.version;
  const warning = document.getElementById('restart-modal-warning');
  // Surface a connection-loss warning if anyone's currently paired & active.
  warning.style.display = (window.__activeConnectionCount || 0) > 0 ? 'block' : 'none';
  document.getElementById('restart-modal').style.display = 'flex';
}

function updatesCloseRestartModal() {
  document.getElementById('restart-modal').style.display = 'none';
}

// Wire modal buttons once on DOMContentLoaded.
document.addEventListener('DOMContentLoaded', () => {
  const confirm = document.getElementById('restart-modal-confirm');
  const cancel = document.getElementById('restart-modal-cancel');
  if (confirm) confirm.addEventListener('click', async () => {
    updatesCloseRestartModal();
    await updatesInstall();
  });
  if (cancel) cancel.addEventListener('click', updatesCloseRestartModal);
  // Lazy initial fetch — once /api/auth/status resolves and the route
  // settles, updatesFetch() pulls the current state. The route() function
  // in common.js fires DOMContentLoaded too, so order doesn't matter.
  setTimeout(updatesFetch, 100);

  // Wire pref-dirty change listeners.
  ['settings-channel', 'settings-autoCheck', 'settings-autoDownload',
   'settings-autoInstall'].forEach(id => {
    const el = document.getElementById(id);
    if (!el) return;
    el.addEventListener('change', () => {
      // When the user toggles auto-check off, auto-download/install become moot.
      const ac = document.getElementById('settings-autoCheck');
      const ad = document.getElementById('settings-autoDownload');
      const ai = document.getElementById('settings-autoInstall');
      if (ad && ac) ad.disabled = !ac.checked;
      if (ai && ad) ai.disabled = !ad.checked || ad.disabled;
      updatesCheckPrefsDirty();
    });
  });
});

// ── Tiny helpers (kept local to avoid mutating common.js) ─────────────────
function makeBtn(cls, label, fn) {
  const b = document.createElement('button');
  b.className = 'btn ' + cls;
  b.textContent = label;
  b.addEventListener('click', fn);
  return b;
}
function setText(id, txt) {
  const el = document.getElementById(id);
  if (el) el.textContent = txt;
}
function openExternal(url) {
  if (url) window.open(url, '_blank', 'noopener,noreferrer');
}
function formatBytes(n) {
  if (!n || n < 1024) return (n || 0) + ' B';
  if (n < 1024 * 1024) return (n / 1024).toFixed(1) + ' KB';
  if (n < 1024 * 1024 * 1024) return (n / (1024 * 1024)).toFixed(1) + ' MB';
  return (n / (1024 * 1024 * 1024)).toFixed(2) + ' GB';
}
function formatRelativeEpoch(epoch) {
  if (!epoch) return t('settings.updates.never');
  const ageSec = Math.max(0, Math.floor(Date.now() / 1000 - epoch));
  if (ageSec < 60)    return t('updates.time.just-now');
  if (ageSec < 3600)  return t('updates.time.minutes-ago', [Math.floor(ageSec / 60)]);
  if (ageSec < 86400) return t('updates.time.hours-ago',   [Math.floor(ageSec / 3600)]);
  return                     t('updates.time.days-ago',    [Math.floor(ageSec / 86400)]);
}
// Title for the Error banner. The server sets `failedPhase` to the phase
// the updater was running when it tripped — distinguishing "couldn't even
// reach the update server" from "download/verify/install failed" so the
// banner doesn't imply an install was attempted when only the check ran.
function updateErrorTitle(failedPhase) {
  switch (failedPhase) {
    case UPDATE_STATE_CHECKING:    return t('updates.error.checking');
    case UPDATE_STATE_DOWNLOADING: return t('updates.error.downloading');
    case UPDATE_STATE_VERIFYING:   return t('updates.error.verifying');
    case UPDATE_STATE_INSTALLING:  return t('updates.error.installing');
    default:                       return t('updates.error.default');
  }
}
function formatPlatformId(id) {
  switch (id) {
    case 'windows':         return t('updates.platform.windows');
    case 'macos':           return t('updates.platform.macos');
    case 'linux-appimage':  return t('updates.platform.linux-appimage');
    case 'linux-deb':       return t('updates.platform.linux-deb');
    case 'linux-rpm':       return t('updates.platform.linux-rpm');
    case 'linux-aur':       return t('updates.platform.linux-aur');
    case 'linux-portable':  return t('updates.platform.linux-portable');
    default:                return id || t('updates.platform.unknown');
  }
}
