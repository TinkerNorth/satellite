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

// ── Routing ─────────────────────────────────────────────────────────────────
function navigate(path) {
  window.history.pushState({}, '', path);
  route();
}

function showView(id) {
  ['view-setup', 'view-login', 'view-dashboard', 'view-debug', 'view-logs'].forEach(v => {
    const el = document.getElementById(v);
    if (el) el.style.display = v === id ? 'block' : 'none';
  });
}

async function route() {
  const path = window.location.pathname;

  // Check auth status first
  const { ok, data } = await api('/api/auth/status');
  if (!ok) { showView('view-login'); return; }

  if (!data.configured) {
    if (path !== '/setup') { navigate('/setup'); return; }
    showView('view-setup');
    if (typeof initSetup === 'function') initSetup();
  } else if (!data.authenticated) {
    if (path !== '/login') { navigate('/login'); return; }
    showView('view-login');
    if (typeof initLogin === 'function') initLogin();
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

