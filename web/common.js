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
      const r = await fetch('/api/auth/status', { signal: AbortSignal.timeout(3000) });
      if (r.ok) {
        // Server is back — stop polling and re-route
        stopOfflinePolling();
        isOffline = false;
        // Re-route which will check auth status and either
        // go to login (if expired) or restore the previous view
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

const ALL_VIEWS = ['view-offline', 'view-setup', 'view-login', 'view-dashboard', 'view-debug', 'view-logs', 'view-settings'];

function showView(id) {
  ALL_VIEWS.forEach(v => {
    const el = document.getElementById(v);
    if (el) el.style.display = v === id ? 'block' : 'none';
  });
}

async function route() {
  const path = window.location.pathname;

  // Check auth status first
  const { ok, status, data } = await api('/api/auth/status');

  // Network error (status 0) = server unreachable
  if (!ok && status === 0) {
    showOffline();
    return;
  }

  // Server responded but something else went wrong
  if (!ok) { showView('view-login'); return; }

  if (!data.configured) {
    if (path !== '/setup') { navigate('/setup'); return; }
    showView('view-setup');
    if (typeof initSetup === 'function') initSetup();
  } else if (!data.authenticated) {
    if (path !== '/login') { navigate('/login'); return; }
    showView('view-login');
    if (typeof initLogin === 'function') initLogin();
  } else if (path === '/settings') {
    showView('view-settings');
    if (typeof initSettings === 'function') initSettings();
  } else if (path === '/debug') {
    showView('view-debug');
    if (typeof initDebug === 'function') initDebug();
  } else if (path === '/logs') {
    showView('view-logs');
    if (typeof initLogs === 'function') initLogs();
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

// ── Init ────────────────────────────────────────────────────────────────────
window.addEventListener('popstate', route);
document.addEventListener('DOMContentLoaded', route);

