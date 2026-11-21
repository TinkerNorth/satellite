// diagnostics.js: the Debug page's "Report a problem". Downloads
// GET /api/diagnostics with the person's report added as
// dish-diagnostics-satellite-<yyyyMMdd-HHmmss>.json and points the mail link at
// support@tinkernorth.com with the contract's subject and the person's words
// first; the person attaches the file themselves.

const DIAGNOSTICS_SUPPORT = 'support@tinkernorth.com';

// docs/diagnostics-bundle.md "The report"; core/diagnostics_bundle.h holds the
// same list and the dashboard contract test holds the two together.
const REPORT_CATEGORIES = ['notRecognised', 'noFeedback', 'connection', 'audio', 'other', 'question'];

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

function reportCategoryLabel(category) {
  return t('report.category.' + category);
}

function readReport() {
  const select = document.getElementById('debug-report-category');
  const text = document.getElementById('debug-report-text');
  const picked = select ? select.value : 'other';
  const category = REPORT_CATEGORIES.includes(picked) ? picked : 'other';
  return { category, text: text ? text.value.trim() : '', controller: null };
}

// The report rides third, after the envelope's own two fields, so a reader
// opening the file sees the person's words first.
function withReport(bundle, report) {
  return Object.assign({ bundle: bundle.bundle, createdAt: bundle.createdAt, report }, bundle);
}

function diagnosticsMailto(bundle, fileName) {
  const report = bundle.report;
  const subject = t('mail.support-subject',
                    [reportCategoryLabel(report.category), bundle.app.version, bundle.device.os]);
  const words = report.text ? [report.text, ''] : [];
  const body = words
    .concat([t('mail.attach-instruction', [fileName]), ''])
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
  const status = document.getElementById('debug-report-status');
  const mail = document.getElementById('debug-report-mail');
  const res = await api('/api/diagnostics');
  if (!res.ok) {
    if (status) status.textContent = t('debug.report.failed');
    return;
  }
  const bundle = withReport(res.data, readReport());
  const fileName = diagnosticsFileName(bundle, new Date());
  diagnosticsDownload(bundle, fileName);
  const href = diagnosticsMailto(bundle, fileName);
  if (mail) mail.href = href;
  if (status) status.textContent = t('debug.report.saved', [fileName]);
}
