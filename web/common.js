// ── common.js — Shared utilities: loading overlay, fetch helpers, routing ────

// ── Loading overlay ─────────────────────────────────────────────────────────
function showLoading() {
  document.getElementById('loading-overlay').classList.add('active');
}

function hideLoading() {
  document.getElementById('loading-overlay').classList.remove('active');
}

// ── Fetch wrapper with loading state ────────────────────────────────────────
async function api(url, opts = {}) {
  showLoading();
  try {
    const r = await fetch(url, opts);
    const data = await r.json();
    return { ok: r.ok, status: r.status, data };
  } catch (e) {
    return { ok: false, status: 0, data: { error: 'Network error' } };
  } finally {
    hideLoading();
  }
}

async function apiPost(url, body = {}) {
  return api(url, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body)
  });
}

// ── Offline detection & reconnection ────────────────────────────────────────
let offlinePollingTimer = null;
let isOffline = false;
// Remember the last path the user was on before going offline
let lastPathBeforeOffline = null;

function showOffline() {
  if (isOffline) return;
  isOffline = true;
  // Stop any active SSE connections
  if (typeof stopSSE === 'function') stopSSE();
  lastPathBeforeOffline = window.location.pathname;
  showView('view-offline');
  startOfflinePolling();
}

function startOfflinePolling() {
  stopOfflinePolling();
  offlinePollingTimer = setInterval(async () => {
    try {
      const r = await fetch('/api/status', { signal: AbortSignal.timeout(3000) });
      if (r.ok) {
        // Server is back — stop polling and restore the previous view
        stopOfflinePolling();
        isOffline = false;
        const restorePath = lastPathBeforeOffline || '/dashboard';
        lastPathBeforeOffline = null;
        window.history.replaceState({}, '', restorePath);
        route();
      }
    } catch (e) {
      // Still offline — keep polling
    }
  }, 3000);
}

function stopOfflinePolling() {
  if (offlinePollingTimer) {
    clearInterval(offlinePollingTimer);
    offlinePollingTimer = null;
  }
}

// ── Routing ─────────────────────────────────────────────────────────────────
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

// The admin UI is bound to localhost and has no authentication — routing is a
// straight path → view switch.
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

// ── HTML escape ─────────────────────────────────────────────────────────────
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
  html += netInfoRow(t('netinfo.port.pair'), port(p.pair));
  html += netInfoRow(t('netinfo.port.client'), port(p.client));
  html += netInfoRow(t('netinfo.port.discovery'), port(p.discovery));
  html += netInfoRow(t('netinfo.port.mdns'), port(p.mdns));
  el.innerHTML = html;
}

// ── Init ────────────────────────────────────────────────────────────────────
// Defer route() until the i18n catalog is in hand so the init functions
// (initDashboard / initSettings / initDebug / initLogs) see translated
// strings on first paint rather than raw keys. If the i18n shim isn't on
// the page (defensive — it always is via index.html), fall through to the
// plain DOMContentLoaded path so we don't deadlock on boot.
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

