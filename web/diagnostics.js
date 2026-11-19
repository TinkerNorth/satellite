// diagnostics.js: the Debug page's "Save diagnostics". Downloads
// GET /api/diagnostics as dish-diagnostics-satellite-<yyyyMMdd-HHmmss>.json and
// points the mail link at support@tinkernorth.com with the contract's subject
// and a summary; the person attaches the file themselves.

const DIAGNOSTICS_SUPPORT = 'support@tinkernorth.com';

function diagnosticsStamp(d) {
  const p = (n) => String(n).padStart(2, '0');
  return d.getFullYear() + p(d.getMonth() + 1) + p(d.getDate()) + '-' +
         p(d.getHours()) + p(d.getMinutes()) + p(d.getSeconds());
}

function diagnosticsFileName(bundle, now) {
  return 'dish-diagnostics-' + bundle.app.name + '-' + diagnosticsStamp(now) + '.json';
}

function diagnosticsDbLine(db) {
  const source = t('settings.controller-db.source.' + db.source);
  const last = db.lastCheckAt
    ? db.lastCheckAt + ' · ' + t('settings.controller-db.result.' + db.lastCheckResult)
    : t('settings.controller-db.never');
  return t('mail.summary.db', [db.bundledVersion, db.activeVersion, source, last]);
}

function diagnosticsSummary(bundle) {
  const lines = [
    t('mail.summary.app', [bundle.app.name, bundle.app.version, bundle.app.flavor]),
    t('mail.summary.device', [bundle.device.os, bundle.device.arch]),
    diagnosticsDbLine(bundle.controllerDb),
    t('mail.summary.hosts', [bundle.hosts.length]),
  ];
  bundle.controllers.forEach((c) => {
    lines.push(t('mail.summary.controller', [c.host, c.ctrlIdx, c.type, c.backend || '—']));
  });
  return lines;
}

function diagnosticsMailto(bundle, fileName) {
  const subject = t('mail.subject', [bundle.app.name, bundle.app.version, bundle.device.model]);
  const body = [t('mail.attach-instruction', [fileName]), '']
    .concat(diagnosticsSummary(bundle))
    .concat(['', t('mail.attachment')])
    .join('\n');
  return 'mailto:' + DIAGNOSTICS_SUPPORT + '?subject=' + encodeURIComponent(subject) +
         '&body=' + encodeURIComponent(body);
}

function diagnosticsDownload(bundle, fileName) {
  const blob = new Blob([JSON.stringify(bundle, null, 2)], { type: 'application/json' });
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  a.download = fileName;
  document.body.appendChild(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}

async function diagnosticsSave() {
  const status = document.getElementById('debug-diagnostics-status');
  const mail = document.getElementById('debug-diagnostics-mail');
  const res = await api('/api/diagnostics');
  if (!res.ok) {
    if (status) status.textContent = t('debug.diagnostics.failed');
    return;
  }
  const fileName = diagnosticsFileName(res.data, new Date());
  diagnosticsDownload(res.data, fileName);
  if (mail) mail.href = diagnosticsMailto(res.data, fileName);
  if (status) status.textContent = t('debug.diagnostics.saved', [fileName]);
}
