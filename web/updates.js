const UPDATE_STATE_IDLE        = 'idle';
const UPDATE_STATE_CHECKING    = 'checking';
const UPDATE_STATE_UP_TO_DATE  = 'up-to-date';
const UPDATE_STATE_AVAILABLE   = 'update-available';
const UPDATE_STATE_DOWNLOADING = 'downloading';
const UPDATE_STATE_VERIFYING   = 'verifying';
const UPDATE_STATE_DOWNLOADED  = 'downloaded';
const UPDATE_STATE_INSTALLING  = 'installing';
const UPDATE_STATE_ERROR       = 'error';

const UPDATE_SURFACE_DASHBOARD = 'dashboard';
const UPDATE_SURFACE_SETTINGS  = 'settings';
const UPDATE_SLOT_DASHBOARD    = 'dashboard-update-slot';
const UPDATE_SLOT_SETTINGS     = 'settings-update-slot';
const UPDATE_ORIGIN_CHECK      = 'settings-btnCheck';
const UPDATE_ORIGIN_PREFS      = 'settings-btnUpdatePrefs';
const UPDATE_ORIGIN_DRIVERS    = 'driver-banner';

const UPDATE_STALE_TICK_MS     = 1500;
const UPDATE_STALL_MS          = 15000;
const UPDATE_COPIED_MS         = 2000;
const UPDATE_POST_TIMEOUT_MS   = 10000;
const UPDATE_GET_TIMEOUT_MS    = 5000;
const UPDATE_STICKY_MAX_MS     = 120000;
const UPDATE_RESTART_SLOW_MS   = 120000;
const UPDATE_MARKER_MAX_AGE_MS = 30 * 60 * 1000;
const UPDATE_MARKER_KEY        = 'satellite-update-restart';
const UPDATE_SPINNER_PX        = 14;

const UPDATE_SLOTS = [
  { slotId: UPDATE_SLOT_DASHBOARD, surface: UPDATE_SURFACE_DASHBOARD },
  { slotId: UPDATE_SLOT_SETTINGS,  surface: UPDATE_SURFACE_SETTINGS },
];

const UPDATE_RESULT_STATES        = [UPDATE_STATE_IDLE, UPDATE_STATE_UP_TO_DATE];
const UPDATE_RESULT_KEEP_STATES   = [UPDATE_STATE_IDLE, UPDATE_STATE_UP_TO_DATE, UPDATE_STATE_CHECKING];
const UPDATE_CHECK_BLOCKED_STATES = [UPDATE_STATE_DOWNLOADING, UPDATE_STATE_VERIFYING, UPDATE_STATE_INSTALLING];
const UPDATE_PREF_IDS = ['settings-channel', 'settings-autoCheck', 'settings-autoDownload', 'settings-autoInstall'];

const UPDATE_STICKY_STATES = new Map([
  ['cancel',       UPDATE_STATE_DOWNLOADING],
  ['repair',       UPDATE_STATE_CHECKING],
  ['result-retry', UPDATE_STATE_CHECKING],
]);

const UPDATE_VIEWS = new Map([
  [UPDATE_STATE_IDLE,        updatesViewIdle],
  [UPDATE_STATE_CHECKING,    updatesViewChecking],
  [UPDATE_STATE_UP_TO_DATE,  updatesViewUpToDate],
  [UPDATE_STATE_AVAILABLE,   updatesViewAvailable],
  [UPDATE_STATE_DOWNLOADING, updatesViewDownloading],
  [UPDATE_STATE_VERIFYING,   updatesViewVerifying],
  [UPDATE_STATE_DOWNLOADED,  updatesViewDownloaded],
  [UPDATE_STATE_INSTALLING,  updatesViewInstalling],
  [UPDATE_STATE_ERROR,       updatesViewError],
]);

const UPDATE_ACTIONS = new Map([
  ['download',       updatesDownloadClicked],
  ['cancel',         updatesCancelClicked],
  ['restart',        updatesRestartClicked],
  ['notes',          updatesNotesClicked],
  ['dismiss',        updatesDismissClicked],
  ['skip',           updatesSkipClicked],
  ['retry',          updatesRetryClicked],
  ['copy',           updatesCopyClicked],
  ['result-dismiss', updatesForgetResult],
  ['result-retry',   updatesResultRetryClicked],
]);

let updatesState = null;
let updatesBooted = false;
let updatesBootVersion = '';
let updatesReloading = false;
let updatesBusy = null;
let updatesInline = null;
let updatesStaleGuard = null;
let updatesProgressMark = null;
let updatesCopiedSlot = '';
let updatesCopiedTimer = null;
let updatesSkipNotice = null;
let updatesResult = null;
let updatesRestartOrigin = UPDATE_SLOT_SETTINGS;

async function updatesFetch() {
  const res = await apiRequest('/api/updates/status', { signal: AbortSignal.timeout(UPDATE_GET_TIMEOUT_MS) });
  if (res.ok && updatesIsSnapshot(res.data)) updatesReceive(res.data);
}

function updatesHandleSSE(snapshot) {
  if (updatesIsSnapshot(snapshot)) updatesReceive(snapshot);
}

function updatesIsSnapshot(d) {
  const hasState = !!d && typeof d.state === 'string';
  return hasState && !!d.info && typeof d.info === 'object';
}

function updatesReceive(s) {
  if (!updatesIsStaleTick(s)) updatesAccept(s);
}

function updatesIsStaleTick(s) {
  const guard = updatesStaleGuard;
  const guarding = !!guard && Date.now() < guard.until;
  return guarding && updatesSignature(s) === guard.signature;
}

function updatesSignature(s) {
  return [s.state, s.bytesDownloaded, !!s.dismissed, s.failedPhase, s.info.version, s.lastCheckEpoch].join('|');
}

function updatesAccept(s) {
  if (updatesReloading) return;
  if (updatesServerReplaced(s)) {
    updatesReloadForNewVersion(s);
    return;
  }
  const firstAfterLoad = !updatesBooted;
  updatesRememberBoot(s);
  updatesState = s;
  updatesTrackProgress(s);
  updatesTrackRestart(s, firstAfterLoad);
  updatesExpireNotes(s);
  updatesSettleBusy(s);
  updatesRenderAll();
}

function updatesServerReplaced(s) {
  return updatesBooted && s.currentVersion !== updatesBootVersion;
}

function updatesRememberBoot(s) {
  if (updatesBooted) return;
  updatesBooted = true;
  updatesBootVersion = s.currentVersion;
}

function updatesTrackProgress(s) {
  if (s.state !== UPDATE_STATE_DOWNLOADING) {
    updatesProgressMark = null;
    return;
  }
  const moved = !updatesProgressMark || updatesProgressMark.bytes !== s.bytesDownloaded;
  if (moved) updatesProgressMark = { bytes: s.bytesDownloaded, at: Date.now() };
}

function updatesDownloadStalled() {
  const mark = updatesProgressMark;
  return !!mark && Date.now() - mark.at >= UPDATE_STALL_MS;
}

function updatesTrackRestart(s, firstAfterLoad) {
  if (s.state === UPDATE_STATE_INSTALLING) {
    updatesWriteMarker(updatesInstallMarker(s));
    return;
  }
  const marker = updatesReadMarker();
  if (!marker) return;
  updatesRemoveMarker();
  if (firstAfterLoad) updatesResult = updatesResultFrom(marker, s);
}

function updatesExpireNotes(s) {
  if (updatesSkipNotice && updatesSkipNotice.state !== s.state) updatesSkipNotice = null;
  if (updatesInline && updatesInline.state !== s.state) updatesInline = null;
  if (updatesResult && !updatesResultSurvives(s)) updatesResult = null;
}

function updatesResultSurvives(s) {
  return UPDATE_RESULT_KEEP_STATES.includes(s.state) || updatesIsCheckFailure(s);
}

function updatesIsCheckFailure(s) {
  return s.state === UPDATE_STATE_ERROR && s.failedPhase === UPDATE_STATE_CHECKING;
}

function updatesSettleBusy(s) {
  const busy = updatesBusy;
  if (!busy || !busy.sticky || updatesBusyHolds(busy, s)) return;
  updatesBusy = null;
  const followedCheck = UPDATE_STICKY_STATES.get(busy.act) === UPDATE_STATE_CHECKING;
  const checkFailed = followedCheck && updatesIsCheckFailure(s);
  if (checkFailed) updatesNoteCheckFailure(busy, s);
  if (busy.act === 'result-retry' && !checkFailed) updatesResult = null;
}

function updatesBusyHolds(busy, s) {
  const holding = s.state === UPDATE_STICKY_STATES.get(busy.act);
  const expired = Date.now() - busy.since > UPDATE_STICKY_MAX_MS;
  return holding && !expired;
}

function updatesNoteCheckFailure(busy, s) {
  const reason = s.message || t('updates.error.unknown');
  const text = t('updates.error.check-failed', [reason]);
  updatesInline = { origin: busy.origin, act: busy.act, state: s.state, text };
}

function updatesMount(slotId) {
  const slot = document.getElementById(slotId);
  if (!slot) return null;
  if (!slot.firstElementChild) updatesBuildBanner(slot);
  return slot.firstElementChild;
}

function updatesBuildBanner(slot) {
  const tpl = document.getElementById('tpl-update-banner');
  if (!tpl) return;
  slot.appendChild(tpl.content.firstElementChild.cloneNode(true));
  slot.addEventListener('click', updatesOnSlotClick);
}

function updatesRenderAll() {
  UPDATE_SLOTS.forEach(updatesRenderSlot);
  updatesRenderCheckButton();
  updatesRenderSettingsFields();
  updatesRenderPrefsButton();
  updatesRenderPrefsStatus();
  updatesRenderRestartConfirm();
  if (typeof renderDriverBanner === 'function') renderDriverBanner();
}

function updatesRenderSlot(slot) {
  const banner = updatesMount(slot.slotId);
  if (!banner) return;
  const ui = updatesUiFor(slot.slotId);
  const model = updatesState
    ? updatesViewModel(slot.surface, updatesState, ui)
    : updatesStatelessModel(slot.surface, ui);
  updatesPatch(banner, model);
}

function updatesUiFor(slotId) {
  const busy = updatesBusy;
  return {
    busyAct: busy && busy.origin === slotId ? busy.act : '',
    locked: busy !== null,
    cancelling: !!busy && busy.act === 'cancel',
    alert: updatesInlineTextFor(slotId),
    resultPinned: updatesResultPinnedIn(slotId),
    copied: updatesCopiedSlot === slotId,
    stalled: updatesDownloadStalled(),
    result: updatesResult,
    skipped: updatesSkipNotice,
  };
}

function updatesInlineTextFor(place) {
  const note = updatesInline;
  return note && updatesInlinePlace(note.origin) === place ? note.text : '';
}

function updatesInlinePlace(origin) {
  return origin === UPDATE_ORIGIN_CHECK ? UPDATE_SLOT_SETTINGS : origin;
}

function updatesResultPinnedIn(slotId) {
  const busy = updatesBusy;
  const note = updatesInline;
  const retrying = !!busy && busy.origin === slotId && busy.act === 'result-retry';
  const retryFailed = !!note && note.origin === slotId && note.act === 'result-retry';
  return retrying || retryFailed;
}

function updatesActionBusy(origin, act) {
  const busy = updatesBusy;
  return !!busy && busy.origin === origin && busy.act === act;
}

function updatesActionsLocked() {
  return updatesBusy !== null;
}

function updatesViewModel(surface, s, ui) {
  return updatesFinishModel(updatesBaseView(surface, s, ui), ui);
}

function updatesStatelessModel(surface, ui) {
  return updatesFinishModel(updatesQuietView(surface, ui, UPDATE_STATE_IDLE), ui);
}

function updatesFinishModel(base, ui) {
  return Object.assign(base, {
    alert: base.visible ? ui.alert : '',
    actions: base.actions.map(a => updatesLockAction(a, ui)),
  });
}

function updatesBaseView(surface, s, ui) {
  if (ui.skipped && surface === UPDATE_SURFACE_SETTINGS) return updatesViewSkipped(ui.skipped);
  if (ui.result && updatesShowsResult(s, ui)) return updatesViewResult(ui.result);
  const view = UPDATE_VIEWS.get(s.state);
  return view ? view(surface, s, ui) : updatesHidden(s.state);
}

function updatesShowsResult(s, ui) {
  return UPDATE_RESULT_STATES.includes(s.state) || ui.resultPinned;
}

function updatesModel(key, fields) {
  return Object.assign({
    key,
    visible: true,
    tone: 'info',
    title: '',
    detail: '',
    code: '',
    progress: null,
    hint: '',
    alert: '',
    actions: [],
  }, fields);
}

function updatesHidden(key) {
  return updatesModel(key, { visible: false });
}

function updatesQuietView(surface, ui, key) {
  const failedCheck = surface === UPDATE_SURFACE_SETTINGS && ui.alert !== '';
  if (!failedCheck) return updatesHidden(key);
  return updatesModel(key, { tone: 'error', title: t('updates.error.checking') });
}

function updatesViewIdle(surface, s, ui) {
  return updatesQuietView(surface, ui, s.state);
}

function updatesViewChecking(surface, s) {
  if (surface !== UPDATE_SURFACE_SETTINGS) return updatesHidden(s.state);
  return updatesModel(s.state, {
    title: t('updates.state.checking.title'),
    progress: updatesIndeterminate(''),
  });
}

function updatesViewUpToDate(surface, s) {
  if (surface !== UPDATE_SURFACE_SETTINGS) return updatesHidden(s.state);
  return updatesModel(s.state, {
    tone: 'success',
    title: t('updates.state.uptodate.title'),
    detail: t('updates.state.uptodate.detail', [s.currentVersion]),
  });
}

function updatesViewAvailable(surface, s, ui) {
  if (updatesDismissedHere(surface, s)) return updatesHidden(s.state);
  const manual = s.info.installMethod === 'manual';
  return manual ? updatesViewManual(surface, s, ui) : updatesViewOffer(surface, s);
}

function updatesDismissedHere(surface, s) {
  return surface === UPDATE_SURFACE_DASHBOARD && !!s.dismissed;
}

function updatesViewOffer(surface, s) {
  return updatesModel(s.state, {
    title: t('updates.state.available.title', [s.info.version]),
    detail: updatesAssetLine(s),
    actions: [
      updatesAction('download', t('updates.btn.download'), true),
      ...updatesNotesActions(s.info.htmlUrl),
      ...updatesDashboardActions(surface, 'dismiss', t('updates.btn.remind-later')),
      updatesAction('skip', t('updates.btn.skip-version'), false),
    ],
  });
}

function updatesViewManual(surface, s, ui) {
  const command = s.info.manualInstruction || '';
  const copyLabel = ui.copied ? t('updates.btn.copied') : t('updates.btn.copy-command');
  const copy = command ? [updatesAction('copy', copyLabel, true)] : [];
  return updatesModel(s.state, {
    title: t('updates.state.available.title', [s.info.version]),
    detail: command ? t('updates.state.available.manual') : '',
    code: command,
    actions: [
      ...copy,
      ...updatesNotesActions(s.info.htmlUrl),
      ...updatesDashboardActions(surface, 'dismiss', t('updates.btn.remind-later')),
    ],
  });
}

function updatesAssetLine(s) {
  const size = s.info.assetSize ? ' · ' + formatBytes(s.info.assetSize) : '';
  return (s.info.assetName || '') + size;
}

function updatesViewDownloading(surface, s, ui) {
  const cancelLabel = ui.cancelling ? t('updates.btn.cancelling') : t('updates.btn.cancel');
  return updatesModel(s.state, {
    title: t('updates.state.downloading.title', [s.info.version]),
    progress: s.totalBytes > 0 ? updatesSizedProgress(s) : updatesOpenProgress(s),
    hint: ui.stalled ? t('updates.state.downloading.stalled') : '',
    actions: [updatesAction('cancel', cancelLabel, false)],
  });
}

function updatesSizedProgress(s) {
  const pct = Math.min(100, Math.floor(100 * s.bytesDownloaded / s.totalBytes));
  const label = t('updates.state.downloading.detail',
                  [formatBytes(s.bytesDownloaded), formatBytes(s.totalBytes)]);
  return { indeterminate: false, pct, label };
}

function updatesOpenProgress(s) {
  return updatesIndeterminate(t('updates.state.downloading.received', [formatBytes(s.bytesDownloaded)]));
}

function updatesIndeterminate(label) {
  return { indeterminate: true, pct: 0, label };
}

function updatesViewVerifying(surface, s) {
  return updatesModel(s.state, {
    title: t('updates.state.verifying.title'),
    detail: t('updates.state.verifying.detail', [s.info.assetName]),
    progress: updatesIndeterminate(''),
  });
}

function updatesViewDownloaded(surface, s) {
  if (updatesDismissedHere(surface, s)) return updatesHidden(s.state);
  return updatesIsReinstall(s) ? updatesViewReadyToReinstall(surface, s) : updatesViewReadyToInstall(surface, s);
}

function updatesViewReadyToInstall(surface, s) {
  return updatesModel(s.state, {
    title: t('updates.state.downloaded.title', [s.info.version]),
    detail: t('updates.state.downloaded.detail'),
    actions: [
      updatesAction('restart', t('updates.btn.restart-install'), true),
      ...updatesDashboardActions(surface, 'dismiss', t('updates.btn.install-later')),
    ],
  });
}

function updatesViewReadyToReinstall(surface, s) {
  return updatesModel(s.state, {
    title: t('updates.state.downloaded.reinstall-title', [s.info.version]),
    detail: t('updates.state.downloaded.reinstall-detail'),
    actions: [
      updatesAction('restart', t('updates.btn.restart-reinstall'), true),
      ...updatesDashboardActions(surface, 'dismiss', t('updates.btn.install-later')),
    ],
  });
}

function updatesIsReinstall(s) {
  return s.info.version === s.currentVersion;
}

function updatesViewInstalling(surface, s) {
  const title = updatesIsReinstall(s)
    ? t('updates.state.installing.reinstall-title', [s.info.version])
    : t('updates.state.installing.title', [s.info.version]);
  return updatesModel(s.state, {
    title,
    detail: t('updates.state.installing.detail'),
    progress: updatesIndeterminate(''),
  });
}

function updatesViewError(surface, s) {
  const hiddenHere = surface === UPDATE_SURFACE_DASHBOARD && updatesIsCheckFailure(s);
  if (hiddenHere) return updatesHidden(s.state);
  return updatesModel(s.state, {
    tone: 'error',
    title: updateErrorTitle(s.failedPhase),
    detail: s.message || t('updates.error.unknown'),
    hint: updateErrorHint(s.failedPhase),
    actions: [updatesAction('retry', t('updates.btn.try-again'), false)],
  });
}

function updatesViewResult(result) {
  return result.updated ? updatesViewUpdated(result) : updatesViewNotFinished(result);
}

function updatesViewUpdated(result) {
  const title = result.reinstall
    ? t('updates.result.reinstalled.title', [result.version])
    : t('updates.result.updated.title', [result.version]);
  return updatesModel('result-updated', {
    tone: 'success',
    title,
    detail: t('updates.result.updated.detail', [result.version]),
    actions: [
      ...updatesNotesActions(result.notes),
      updatesAction('result-dismiss', t('updates.btn.dismiss'), false),
    ],
  });
}

function updatesViewNotFinished(result) {
  return updatesModel('result-failed', {
    tone: 'error',
    title: t('updates.result.failed.title', [result.version]),
    detail: t('updates.result.failed.detail', [result.running]),
    actions: [
      updatesAction('result-retry', t('updates.btn.try-again'), false),
      updatesAction('result-dismiss', t('updates.btn.dismiss'), false),
    ],
  });
}

function updatesViewSkipped(notice) {
  return updatesModel('notice-skipped', {
    title: t('updates.notice.skipped.title', [notice.version]),
    detail: t('updates.notice.skipped.detail'),
  });
}

function updatesAction(act, label, primary, url) {
  return { act, label, primary, url: url || '' };
}

function updatesNotesActions(url) {
  return url ? [updatesAction('notes', t('updates.btn.view-notes'), false, url)] : [];
}

function updatesDashboardActions(surface, act, label) {
  return surface === UPDATE_SURFACE_DASHBOARD ? [updatesAction(act, label, false)] : [];
}

function updatesLockAction(a, ui) {
  return Object.assign({}, a, { busy: a.act === ui.busyAct, disabled: ui.locked });
}

function updatesPatch(banner, model) {
  updatesPatchAttr(banner, 'data-state', model.key);
  banner.classList.toggle('update-banner-error', model.tone === 'error');
  banner.classList.toggle('update-banner-success', model.tone === 'success');
  patchDisplay(banner, model.visible ? 'flex' : 'none');
  updatesPatchText(banner.querySelector('.update-banner-title'), model.title);
  updatesPatchText(banner.querySelector('.update-banner-detail'), model.detail);
  updatesPatchText(banner.querySelector('.update-manual-cmd'), model.code);
  updatesPatchProgress(banner.querySelector('.update-banner-progress'), model.progress, model.title);
  updatesPatchText(banner.querySelector('.update-banner-hint'), model.hint);
  patchText(banner.querySelector('.update-banner-alert'), model.alert);
  updatesPatchActions(banner.querySelector('.update-banner-actions'), model.actions);
}

function updatesPatchText(el, text) {
  patchText(el, text);
  updatesPatchHidden(el, text === '');
}

function updatesPatchHidden(el, hidden) {
  if (el && el.hidden !== hidden) el.hidden = hidden;
}

function updatesPatchAttr(el, name, value) {
  if (value === null) {
    if (el.hasAttribute(name)) el.removeAttribute(name);
    return;
  }
  if (el.getAttribute(name) !== value) el.setAttribute(name, value);
}

function updatesPatchProgress(el, progress, title) {
  if (!el) return;
  updatesPatchHidden(el, progress === null);
  if (progress === null) return;
  const pctText = progress.indeterminate ? '' : progress.pct + '%';
  const valueNow = progress.indeterminate ? null : String(progress.pct);
  const valueText = progress.indeterminate && progress.label ? progress.label : null;
  el.classList.toggle('update-banner-progress-indeterminate', progress.indeterminate);
  updatesPatchAttr(el, 'aria-label', title);
  updatesPatchAttr(el, 'aria-valuenow', valueNow);
  updatesPatchAttr(el, 'aria-valuetext', valueText);
  const fill = el.querySelector('.update-banner-progress-fill');
  const width = progress.indeterminate ? '' : pctText;
  if (fill && fill.style.width !== width) fill.style.width = width;
  updatesPatchText(el.querySelector('.update-banner-progress-label'), progress.label);
  updatesPatchText(el.querySelector('.update-banner-progress-pct'), pctText);
  updatesPatchHidden(el.querySelector('.update-banner-progress-text'), progress.label === '' && pctText === '');
}

function updatesPatchActions(container, actions) {
  if (!container) return;
  const keys = actions.map(a => a.act).join(' ');
  if (container.__acts !== keys) {
    container.replaceChildren(...actions.map(updatesCreateButton));
    container.__acts = keys;
  }
  actions.forEach((a, i) => updatesPatchButton(container.children[i], a));
}

function updatesCreateButton(a) {
  const b = document.createElement('button');
  b.type = 'button';
  b.dataset.act = a.act;
  return b;
}

function updatesPatchButton(b, a) {
  const cls = a.primary ? 'btn btn-start' : 'btn btn-undo';
  if (b.className !== cls) b.className = cls;
  updatesPatchButtonContent(b, a.label, a.busy);
  if (b.disabled !== a.disabled) b.disabled = a.disabled;
  updatesPatchAttr(b, 'data-url', a.url ? a.url : null);
}

function updatesPatchButtonContent(b, label, busy) {
  const content = (busy ? 'busy:' : 'idle:') + label;
  if (b.__content === content) return;
  b.__content = content;
  if (busy) b.innerHTML = buttonLoaderHTML(UPDATE_SPINNER_PX, label);
  else b.textContent = label;
  updatesPatchAttr(b, 'aria-busy', busy ? 'true' : null);
}

function updatesRenderCheckButton() {
  const btn = document.getElementById(UPDATE_ORIGIN_CHECK);
  if (!btn) return;
  const s = updatesState;
  const posting = updatesActionBusy(UPDATE_ORIGIN_CHECK, 'check');
  const lockedElsewhere = updatesActionsLocked() && !posting;
  const checking = posting || (!lockedElsewhere && !!s && s.state === UPDATE_STATE_CHECKING);
  const blocked = !!s && UPDATE_CHECK_BLOCKED_STATES.includes(s.state);
  const label = checking ? t('settings.updates.checking') : t('settings.updates.check');
  updatesPatchButtonContent(btn, label, checking);
  const disabled = checking || blocked || updatesActionsLocked();
  if (btn.disabled !== disabled) btn.disabled = disabled;
}

// Form controls (channel + auto* toggles) are seeded once per page mount,
// otherwise a 1 Hz SSE re-render would clobber the user's in-progress edits.
let savedPrefs = null;
let formSeeded = false;

function updatesSeedFormOnce() {
  if (formSeeded || !updatesState) return;
  const s = updatesState;
  const ch = document.getElementById('settings-channel');
  const ac = document.getElementById('settings-autoCheck');
  const ad = document.getElementById('settings-autoDownload');
  const ai = document.getElementById('settings-autoInstall');
  if (!ch || !ac || !ad || !ai) return;
  ch.value = s.channel || 'stable';
  ac.checked = !!s.autoCheck;
  ad.checked = !!s.autoDownload;
  ai.checked = !!s.autoInstall;
  updatesApplyToggleState();
  savedPrefs = updatesPrefsFrom(s);
  formSeeded = true;
}

function updatesRenderSettingsFields() {
  if (!updatesState) return;
  const s = updatesState;
  setText('settings-version', s.currentVersion);
  setText('settings-platform', formatPlatformId(s.platformId));
  setText('settings-last-check', s.lastCheckEpoch ? formatRelativeEpoch(s.lastCheckEpoch) : t('settings.updates.never'));
  updatesSeedFormOnce();
  updatesApplyToggleState();
}

function updatesPrefsFrom(s) {
  return {
    channel: s.channel || 'stable',
    autoCheck: !!s.autoCheck,
    autoDownload: !!s.autoDownload,
    autoInstall: !!s.autoInstall,
  };
}

function updatesReadPrefsForm() {
  return {
    channel: document.getElementById('settings-channel')?.value,
    autoCheck: !!document.getElementById('settings-autoCheck')?.checked,
    autoDownload: !!document.getElementById('settings-autoDownload')?.checked,
    autoInstall: !!document.getElementById('settings-autoInstall')?.checked,
  };
}

function updatesPrefsDirty() {
  if (!savedPrefs) return false;
  const cur = updatesReadPrefsForm();
  const channelChanged = cur.channel !== savedPrefs.channel;
  const togglesChanged = cur.autoCheck !== savedPrefs.autoCheck ||
                         cur.autoDownload !== savedPrefs.autoDownload ||
                         cur.autoInstall !== savedPrefs.autoInstall;
  return channelChanged || togglesChanged;
}

function updatesRenderPrefsButton() {
  const btn = document.getElementById(UPDATE_ORIGIN_PREFS);
  if (!btn) return;
  updatesPatchButtonContent(btn, t('settings.updates.save-prefs'), updatesActionBusy(UPDATE_ORIGIN_PREFS, 'prefs'));
  const disabled = updatesActionsLocked() || !updatesPrefsDirty();
  if (btn.disabled !== disabled) btn.disabled = disabled;
}

function updatesRenderPrefsStatus() {
  patchText(document.getElementById('settings-update-prefs-status'), updatesInlineTextFor(UPDATE_ORIGIN_PREFS));
}

function updatesRenderRestartConfirm() {
  const btn = document.getElementById('restart-modal-confirm');
  const locked = updatesActionsLocked();
  if (btn && btn.disabled !== locked) btn.disabled = locked;
}

function updatesOnPrefsInput() {
  updatesApplyToggleState();
  updatesDropPrefsFailure();
  updatesRenderPrefsButton();
  updatesRenderPrefsStatus();
}

function updatesDropPrefsFailure() {
  if (updatesInline && updatesInline.origin === UPDATE_ORIGIN_PREFS) updatesInline = null;
}

async function updatesPost(origin, act, path, body = {}) {
  if (updatesBusy) return null;
  const before = updatesState;
  updatesBusy = { origin, act, sticky: false, since: Date.now() };
  updatesInline = null;
  updatesRenderAll();
  const res = await apiPostQuiet(path, body, UPDATE_POST_TIMEOUT_MS);
  const reply = updatesIsSnapshot(res.data) ? res.data : null;
  if (!reply) {
    updatesBusy = null;
    updatesNoteNoReply(origin, act, before);
    return null;
  }
  updatesBusy = updatesHoldBusy(origin, act, reply);
  updatesApplyReply(before, reply);
  return reply;
}

function updatesHoldBusy(origin, act, s) {
  const holds = s.state === UPDATE_STICKY_STATES.get(act);
  return holds ? { origin, act, sticky: true, since: Date.now() } : null;
}

function updatesApplyReply(before, s) {
  updatesStaleGuard = before
    ? { until: Date.now() + UPDATE_STALE_TICK_MS, signature: updatesSignature(before) }
    : null;
  updatesAccept(s);
}

function updatesNoteNoReply(origin, act, before) {
  const text = origin === UPDATE_ORIGIN_PREFS
    ? t('settings.updates.prefs-failed')
    : t('updates.error.no-response');
  updatesInline = { origin, act, state: before ? before.state : '', text };
  updatesRenderAll();
}

async function updatesCheck() {
  const reply = await updatesPost(UPDATE_ORIGIN_CHECK, 'check', '/api/updates/check');
  if (reply) updatesForgetResult();
}

function updatesInstall() {
  updatesPost(updatesRestartOrigin, 'restart', '/api/updates/install');
}

function updatesRepair() {
  updatesPost(UPDATE_ORIGIN_DRIVERS, 'repair', '/api/updates/repair');
}

async function updatesSavePrefs() {
  const reply = await updatesPost(UPDATE_ORIGIN_PREFS, 'prefs', '/api/updates/preferences', updatesReadPrefsForm());
  if (reply) updatesRememberPrefs(reply);
}

function updatesRememberPrefs(s) {
  savedPrefs = updatesPrefsFrom(s);
  updatesRenderPrefsButton();
}

function updatesOnSlotClick(e) {
  const btn = e.target.closest('button[data-act]');
  if (!btn || btn.disabled) return;
  const handler = UPDATE_ACTIONS.get(btn.dataset.act);
  if (handler) handler(e.currentTarget.id, btn);
}

function updatesDownloadClicked(slotId) {
  updatesPost(slotId, 'download', '/api/updates/download');
}

function updatesCancelClicked(slotId) {
  updatesPost(slotId, 'cancel', '/api/updates/cancel');
}

function updatesDismissClicked(slotId) {
  updatesPost(slotId, 'dismiss', '/api/updates/dismiss');
}

function updatesRetryClicked(slotId) {
  updatesPost(slotId, 'retry', '/api/updates/retry');
}

function updatesRestartClicked(origin) {
  updatesRestartOrigin = origin;
  updatesPromptRestart();
}

function updatesNotesClicked(slotId, btn) {
  openExternal(btn.dataset.url);
}

async function updatesSkipClicked(slotId) {
  const version = updatesSkipTarget(updatesState);
  const reply = await updatesPost(slotId, 'skip', '/api/updates/skip', { version });
  if (reply) updatesNoteSkipped(version, reply);
}

function updatesSkipTarget(s) {
  return s ? s.info.version : '';
}

function updatesNoteSkipped(version, s) {
  updatesSkipNotice = { version, state: s.state };
  updatesRenderAll();
}

function updatesResultRetryClicked(slotId) {
  updatesPost(slotId, 'result-retry', '/api/updates/check');
}

function updatesForgetResult() {
  updatesResult = null;
  updatesRenderAll();
}

async function updatesCopyClicked(slotId) {
  const copied = await updatesWriteClipboard(updatesCommandText(updatesState));
  if (copied) updatesFlashCopied(slotId);
  else updatesSelectCommand(slotId);
}

function updatesCommandText(s) {
  return s ? s.info.manualInstruction || '' : '';
}

async function updatesWriteClipboard(text) {
  try {
    await navigator.clipboard.writeText(text);
    return true;
  } catch (e) {
    return false;
  }
}

function updatesFlashCopied(slotId) {
  clearTimeout(updatesCopiedTimer);
  updatesCopiedSlot = slotId;
  updatesCopiedTimer = setTimeout(updatesEndCopied, UPDATE_COPIED_MS);
  updatesRenderAll();
}

function updatesEndCopied() {
  updatesCopiedSlot = '';
  updatesCopiedTimer = null;
  updatesRenderAll();
}

function updatesSelectCommand(slotId) {
  const slot = document.getElementById(slotId);
  const pre = slot ? slot.querySelector('.update-manual-cmd') : null;
  if (!pre) return;
  const range = document.createRange();
  range.selectNodeContents(pre);
  const selection = window.getSelection();
  selection.removeAllRanges();
  selection.addRange(range);
}

// Called when the user navigates away from /settings so the next visit
// reseeds from the latest server snapshot.
function updatesResetForm() {
  formSeeded = false;
  savedPrefs = null;
  updatesDropPrefsFailure();
}

function updatesPromptRestart() {
  const s = updatesState;
  if (!s) return;
  // Set the body via the i18n catalog so the version is interpolated rather
  // than spliced into hard-coded English markup.
  const body = document.getElementById('restart-modal-body');
  if (body) body.textContent = t('modal.restart.body', [s.info.version]);
  const verEl = document.getElementById('restart-modal-version');
  if (verEl) verEl.textContent = s.info.version;
  const warning = document.getElementById('restart-modal-warning');
  warning.style.display = (window.__activeConnectionCount || 0) > 0 ? 'block' : 'none';
  document.getElementById('restart-modal').style.display = 'flex';
}

function updatesCloseRestartModal() {
  document.getElementById('restart-modal').style.display = 'none';
}

function updatesConfirmRestart() {
  updatesCloseRestartModal();
  updatesInstall();
}

function updatesRestartPending() {
  const s = updatesState;
  const installing = !!s && s.state === UPDATE_STATE_INSTALLING;
  return installing || updatesFreshMarker() !== null;
}

function updatesRestartCopy(elapsedMs) {
  const slow = elapsedMs >= UPDATE_RESTART_SLOW_MS;
  return {
    title: t('updates.restart.title'),
    message: t('updates.restart.message', [updatesRestartVersion()]),
    hint: slow ? t('updates.restart.slow') : t('updates.restart.hint'),
  };
}

function updatesRestartVersion() {
  const marker = updatesFreshMarker();
  if (marker) return marker.to;
  const s = updatesState;
  return s ? s.info.version : '';
}

async function updatesReloadIfRestarted() {
  const hadMarker = updatesReadMarker() !== null;
  const replaced = hadMarker ? '' : await updatesReplacedVersion();
  if (replaced) updatesWriteMarkerOnce(updatesBootVersion, replaced);
  const restarted = hadMarker || replaced !== '';
  if (restarted) updatesReload();
  return restarted;
}

async function updatesReplacedVersion() {
  if (!updatesBooted) return '';
  const version = await updatesServerVersion();
  return version !== '' && version !== updatesBootVersion ? version : '';
}

async function updatesServerVersion() {
  const res = await apiRequest('/api/version', { signal: AbortSignal.timeout(UPDATE_GET_TIMEOUT_MS) });
  const version = res.ok && res.data ? res.data.version : '';
  return typeof version === 'string' ? version : '';
}

function updatesReloadForNewVersion(s) {
  updatesWriteMarkerOnce(updatesBootVersion, s.currentVersion);
  updatesReload();
}

function updatesReload() {
  updatesReloading = true;
  window.location.reload();
}

function updatesInstallMarker(s) {
  return { from: s.currentVersion, to: s.info.version, notes: s.info.htmlUrl, at: Date.now() };
}

function updatesWriteMarkerOnce(from, to) {
  if (updatesReadMarker()) return;
  updatesWriteMarker({ from, to, notes: updatesNotesFor(to), at: Date.now() });
}

function updatesNotesFor(version) {
  const s = updatesState;
  return s && s.info.version === version ? s.info.htmlUrl : '';
}

function updatesReadMarker() {
  try {
    const raw = sessionStorage.getItem(UPDATE_MARKER_KEY);
    const marker = raw ? JSON.parse(raw) : null;
    return updatesValidMarker(marker) ? marker : null;
  } catch (e) {
    return null;
  }
}

function updatesValidMarker(marker) {
  return !!marker && typeof marker.to === 'string' && typeof marker.at === 'number';
}

function updatesFreshMarker() {
  const marker = updatesReadMarker();
  const fresh = !!marker && Date.now() - marker.at <= UPDATE_MARKER_MAX_AGE_MS;
  return fresh ? marker : null;
}

function updatesWriteMarker(marker) {
  try { sessionStorage.setItem(UPDATE_MARKER_KEY, JSON.stringify(marker)); } catch (e) {}
}

function updatesRemoveMarker() {
  try { sessionStorage.removeItem(UPDATE_MARKER_KEY); } catch (e) {}
}

function updatesResultFrom(marker, s) {
  const fresh = Date.now() - marker.at <= UPDATE_MARKER_MAX_AGE_MS;
  if (!fresh) return null;
  return {
    updated: s.currentVersion === marker.to,
    reinstall: marker.from === marker.to,
    version: marker.to,
    running: s.currentVersion,
    notes: typeof marker.notes === 'string' ? marker.notes : '',
  };
}

function updatesWireStatic() {
  const confirm = document.getElementById('restart-modal-confirm');
  const cancel = document.getElementById('restart-modal-cancel');
  if (confirm) confirm.addEventListener('click', updatesConfirmRestart);
  if (cancel) cancel.addEventListener('click', updatesCloseRestartModal);
  UPDATE_PREF_IDS.forEach(updatesWirePrefInput);
  updatesBootFetch();
}

function updatesWirePrefInput(id) {
  const el = document.getElementById(id);
  if (el) el.addEventListener('change', updatesOnPrefsInput);
}

function updatesBootFetch() {
  const i18nReady = !!window.i18n && typeof window.i18n.ready === 'function';
  if (i18nReady) window.i18n.ready().then(updatesFetch);
  else updatesFetch();
}

document.addEventListener('DOMContentLoaded', updatesWireStatic);

function openExternal(url) {
  const web = typeof url === 'string' && /^https?:\/\//i.test(url);
  if (web) window.open(url, '_blank', 'noopener,noreferrer');
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
// failedPhase distinguishes a failed check from a failed download/verify/
// install so the banner doesn't imply an install was attempted.
function updateErrorTitle(failedPhase) {
  switch (failedPhase) {
    case UPDATE_STATE_CHECKING:    return t('updates.error.checking');
    case UPDATE_STATE_DOWNLOADING: return t('updates.error.downloading');
    case UPDATE_STATE_VERIFYING:   return t('updates.error.verifying');
    case UPDATE_STATE_INSTALLING:  return t('updates.error.installing');
    default:                       return t('updates.error.default');
  }
}
function updateErrorHint(failedPhase) {
  switch (failedPhase) {
    case UPDATE_STATE_CHECKING:
    case UPDATE_STATE_DOWNLOADING: return t('updates.error.hint.connection');
    case UPDATE_STATE_VERIFYING:   return t('updates.error.hint.verify');
    case UPDATE_STATE_INSTALLING:  return t('updates.error.hint.install');
    default:                       return '';
  }
}
// Only these three can download and apply an update in place. Everywhere else
// a package manager owns the binary, the server refuses to download
// (UpdateService::requestDownload bails on InstallMethod::Manual), and
// auto-download never arms -- so the two toggles below must not look live.
function platformSelfInstalls(id) {
  return id === 'windows' || id === 'macos' || id === 'linux-appimage';
}

// Auto-check off makes auto-download moot; auto-download off makes
// auto-install moot; and neither means anything without self-install.
function updatesApplyToggleState() {
  const ac = document.getElementById('settings-autoCheck');
  const ad = document.getElementById('settings-autoDownload');
  const ai = document.getElementById('settings-autoInstall');
  const note = document.getElementById('settings-update-managed');
  if (!ac || !ad || !ai) return;
  const selfInstall = platformSelfInstalls(updatesState && updatesState.platformId);
  ad.disabled = !selfInstall || !ac.checked;
  ai.disabled = !selfInstall || !ad.checked || ad.disabled;
  if (note) note.style.display = selfInstall ? 'none' : '';
}

function formatPlatformId(id) {
  switch (id) {
    case 'windows':         return t('updates.platform.windows');
    case 'macos':           return t('updates.platform.macos');
    case 'linux-appimage':  return t('updates.platform.linux-appimage');
    case 'linux-deb':       return t('updates.platform.linux-deb');
    case 'linux-rpm':       return t('updates.platform.linux-rpm');
    case 'linux-aur':       return t('updates.platform.linux-aur');
    case 'linux-snap':      return t('updates.platform.linux-snap');
    case 'linux-flatpak':   return t('updates.platform.linux-flatpak');
    case 'linux-portable':  return t('updates.platform.linux-portable');
    default:                return id || t('updates.platform.unknown');
  }
}
