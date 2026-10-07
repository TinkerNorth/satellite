function showLoading() {
  document.getElementById('loading-overlay').classList.add('active');
}

function hideLoading() {
  document.getElementById('loading-overlay').classList.remove('active');
}

async function apiRequest(url, opts = {}) {
  try {
    const r = await fetch(url, opts);
    const data = await r.json();
    return { ok: r.ok, status: r.status, data };
  } catch (e) {
    return { ok: false, status: 0, data: { error: 'Network error' } };
  }
}

async function api(url, opts = {}) {
  showLoading();
  try {
    return await apiRequest(url, opts);
  } finally {
    hideLoading();
  }
}

function jsonPostOptions(body) {
  return {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body)
  };
}

async function apiPost(url, body = {}) {
  return api(url, jsonPostOptions(body));
}

async function apiPostQuiet(url, body, timeoutMs) {
  const opts = Object.assign(jsonPostOptions(body), { signal: AbortSignal.timeout(timeoutMs) });
  return apiRequest(url, opts);
}

function buttonLoaderHTML(size, label) {
  const text = label ? '<span>' + esc(label) + '</span>' : '';
  return '<span class="btn-with-loader">' + spinnerSVG(size) + text + '</span>';
}

const OFFLINE_POLL_MS = 3000;

let offlinePollingTimer = null;
let isOffline = false;
let locationBeforeOffline = null;
let offlineSince = 0;

function showOffline() {
  if (isOffline) return;
  isOffline = true;
  if (typeof stopSSE === 'function') stopSSE();
  locationBeforeOffline = window.location.pathname + window.location.search;
  offlineSince = Date.now();
  renderOfflineCopy();
  showView('view-offline');
  startOfflinePolling();
}

function startOfflinePolling() {
  stopOfflinePolling();
  offlinePollingTimer = setInterval(pollWhileOffline, OFFLINE_POLL_MS);
}

async function pollWhileOffline() {
  renderOfflineCopy();
  try {
    const r = await fetch('/api/status', { signal: AbortSignal.timeout(OFFLINE_POLL_MS) });
    if (r.ok && isOffline) await reconnectAfterOffline();
  } catch (e) {}
}

async function reconnectAfterOffline() {
  stopOfflinePolling();
  isOffline = false;
  const restoreLocation = locationBeforeOffline || '/dashboard';
  locationBeforeOffline = null;
  window.history.replaceState({}, '', restoreLocation);
  const reloading = typeof updatesReloadIfRestarted === 'function' && await updatesReloadIfRestarted();
  if (!reloading) route();
}

function offlineCopy() {
  const restarting = typeof updatesRestartPending === 'function' && updatesRestartPending();
  return restarting ? updatesRestartCopy(Date.now() - offlineSince) : ordinaryOfflineCopy();
}

function ordinaryOfflineCopy() {
  return { title: t('offline.title'), message: t('offline.message'), hint: t('offline.hint') };
}

function renderOfflineCopy() {
  const copy = offlineCopy();
  setText('offline-title', copy.title);
  setText('offline-message', copy.message);
  setText('offline-hint', copy.hint);
}

function patchText(el, text) {
  if (el && el.textContent !== text) el.textContent = text;
}

function setText(id, text) {
  patchText(document.getElementById(id), text);
}

function patchDisplay(el, display) {
  if (el && el.style.display !== display) el.style.display = display;
}

function stopOfflinePolling() {
  if (offlinePollingTimer) {
    clearInterval(offlinePollingTimer);
    offlinePollingTimer = null;
  }
}

function navigate(path) {
  window.history.pushState({}, '', path);
  route();
}

const ALL_VIEWS = ['view-offline', 'view-dashboard', 'view-debug', 'view-logs', 'view-settings', 'view-donate'];

function showView(id) {
  ALL_VIEWS.forEach(v => {
    const el = document.getElementById(v);
    if (el) el.style.display = v === id ? 'block' : 'none';
  });
  updateDonatePill(id);
}

const DONATE_DISMISS_KEY = 'satellite-donate-dismissed';

function donatePillDismissed() {
  try { return sessionStorage.getItem(DONATE_DISMISS_KEY) === '1'; }
  catch (e) { return false; }
}

function updateDonatePill(viewId) {
  const bar = document.getElementById('donate-bar');
  if (!bar) return;
  const hideHere = viewId === 'view-donate' || viewId === 'view-offline';
  bar.hidden = hideHere || donatePillDismissed();
}

function wireDonatePill() {
  const bar = document.getElementById('donate-bar');
  if (!bar) return;
  const dismiss = bar.querySelector('.donate-bar-dismiss');
  if (dismiss) {
    dismiss.addEventListener('click', e => {
      e.preventDefault();
      e.stopPropagation();
      bar.hidden = true;
      try { sessionStorage.setItem(DONATE_DISMISS_KEY, '1'); } catch (e) {}
    });
  }
}

function route() {
  const path = window.location.pathname;

  if (path === '/settings') {
    showView('view-settings');
    if (typeof initSettings === 'function') initSettings();
  } else if (path === '/debug') {
    showView('view-debug');
    if (typeof initDebug === 'function') initDebug();
  } else if (path === '/logs') {
    showView('view-logs');
    if (typeof initLogs === 'function') initLogs();
  } else if (path === '/donate') {
    showView('view-donate');
    if (typeof initDonate === 'function') initDonate();
  } else {
    if (path !== '/dashboard') { navigate('/dashboard'); return; }
    showView('view-dashboard');
    if (typeof initDashboard === 'function') initDashboard();
  }
}

function esc(s) {
  const d = document.createElement('div');
  d.textContent = s;
  return d.innerHTML;
}

async function getNetInfo() {
  try {
    const r = await fetch('/api/netinfo');
    if (!r.ok) return null;
    return await r.json();
  } catch (e) {
    return null;
  }
}

async function fetchNetInfo(containerId) {
  const d = await getNetInfo();
  if (d) renderNetInfoPanel(containerId, d);
}

function netInfoRow(label, value) {
  return '<div class="stat"><span class="label">' + esc(label) +
         '</span><span class="value">' + esc(value) + '</span></div>';
}

function netInfoStatusRow(label, value, color) {
  const style = color ? ' style="color:' + color + '"' : '';
  return '<div class="stat"><span class="label">' + esc(label) +
         '</span><span class="value"' + style + '>' + esc(value) + '</span></div>';
}

function firewallStatusView(state) {
  if (state === 'configured') return { text: t('netinfo.fw.ok'), color: 'var(--success)' };
  if (state === 'wrong-profile') return { text: t('netinfo.fw.blocked'), color: 'var(--warning)' };
  if (state === 'missing') return { text: t('netinfo.fw.missing'), color: 'var(--warning)' };
  return { text: t('netinfo.unknown'), color: '' };
}

function netCategoryLabel(cat) {
  if (cat === 'private') return t('netinfo.cat.private');
  if (cat === 'domain') return t('netinfo.cat.domain');
  if (cat === 'public') return t('netinfo.cat.public');
  return t('netinfo.unknown');
}

function renderNetInfoPanel(containerId, d) {
  const el = document.getElementById(containerId);
  if (!el || !d) return;
  const p = d.ports || {};
  const unknown = t('netinfo.unknown');
  const port = v => (v == null ? unknown : String(v));
  let html = '';
  html += netInfoRow(t('netinfo.ip'), d.lanIp || unknown);
  html += netInfoRow(t('netinfo.device'), d.device || unknown);
  html += netInfoRow(t('netinfo.category'), netCategoryLabel(d.category));
  if (d.firewall && d.firewall.supported) {
    const fwv = firewallStatusView(d.firewall.state);
    html += netInfoStatusRow(t('netinfo.firewall'), fwv.text, fwv.color);
  }
  if (Array.isArray(d.interfaces)) {
    for (const f of d.interfaces) {
      const tag = f.physical ? '' : ' ' + t('netinfo.virtual');
      html += netInfoRow((f.name || unknown) + tag, f.ip || unknown);
    }
  }
  html += netInfoRow(t('netinfo.port.udp'), port(p.udp));
  html += netInfoRow(t('netinfo.port.web'), port(p.web));
  html += netInfoRow(t('netinfo.port.client'), port(p.client));
  html += netInfoRow(t('netinfo.port.discovery'), port(p.discovery));
  html += netInfoRow(t('netinfo.port.mdns'), port(p.mdns));
  el.innerHTML = html;
}

// Defer route() until the i18n catalog is loaded so init functions see
// translated strings on first paint, not raw keys.
function bootRoute() {
  if (window.i18n && typeof window.i18n.ready === 'function') {
    window.i18n.ready().then(route);
  } else {
    route();
  }
}
window.addEventListener('popstate', route);
document.addEventListener('DOMContentLoaded', wireDonatePill);
document.addEventListener('DOMContentLoaded', bootRoute);

