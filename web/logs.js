// ── logs.js — System log viewer ──────────────────────────────────────────────
let _logTimer = null;
let _logSeq = 0;       // highest seq we've received
let _logEntries = [];   // all entries we have locally

function initLogs() {
  _logSeq = 0;
  _logEntries = [];
  document.getElementById('log-entries').innerHTML = '<p class="hint">Loading logs…</p>';

  // Wire up filter checkboxes and search
  ['log-show-info', 'log-show-warn', 'log-show-error'].forEach(id => {
    document.getElementById(id).onchange = renderLogs;
  });
  document.getElementById('log-search').oninput = renderLogs;

  fetchLogs();
  clearInterval(_logTimer);
  _logTimer = setInterval(fetchLogs, 2000);
}

async function fetchLogs() {
  try {
    const r = await fetch('/api/logs?since=' + _logSeq);
    if (!r.ok) return;
    const data = await r.json();

    if (data.entries && data.entries.length > 0) {
      for (const e of data.entries) {
        _logEntries.push(e);
      }
      // Trim to last 500 locally
      if (_logEntries.length > 500) {
        _logEntries = _logEntries.slice(_logEntries.length - 500);
      }
    }
    _logSeq = data.seq;
    renderLogs();
  } catch (e) { /* ignore */ }
}

function renderLogs() {
  const container = document.getElementById('log-entries');
  const showInfo = document.getElementById('log-show-info').checked;
  const showWarn = document.getElementById('log-show-warn').checked;
  const showError = document.getElementById('log-show-error').checked;
  const search = document.getElementById('log-search').value.toLowerCase();
  const autoScroll = document.getElementById('log-auto-scroll').checked;

  const filtered = _logEntries.filter(e => {
    if (e.level === 'info' && !showInfo) return false;
    if (e.level === 'warn' && !showWarn) return false;
    if (e.level === 'error' && !showError) return false;
    if (search && !(e.message.toLowerCase().includes(search) ||
                     e.source.toLowerCase().includes(search))) return false;
    return true;
  });

  if (filtered.length === 0) {
    container.innerHTML = '<p class="hint">No log entries match filters</p>';
    return;
  }

  let html = '';
  for (const e of filtered) {
    const ts = new Date(e.ts).toLocaleTimeString('en-US', { hour12: false, hour: '2-digit', minute: '2-digit', second: '2-digit' });
    const levelClass = e.level === 'error' ? 'log-error' : e.level === 'warn' ? 'log-warn' : 'log-info';
    const levelBadge = e.level === 'error' ? 'ERR' : e.level === 'warn' ? 'WRN' : 'INF';
    html += `<div class="log-line ${levelClass}">` +
            `<span class="log-ts">${ts}</span>` +
            `<span class="log-badge ${levelClass}">${levelBadge}</span>` +
            `<span class="log-source">${esc(e.source)}</span>` +
            `<span class="log-msg">${esc(e.message)}</span>` +
            `</div>`;
  }
  container.innerHTML = html;

  if (autoScroll) {
    const logContainer = document.getElementById('log-container');
    logContainer.scrollTop = logContainer.scrollHeight;
  }
}

// Stop polling when leaving the page
const _origShowView = showView;
showView = function(id) {
  if (id !== 'view-logs' && _logTimer) {
    clearInterval(_logTimer);
    _logTimer = null;
  }
  _origShowView(id);
};

