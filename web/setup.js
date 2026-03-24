// ── setup.js — First-launch setup page ──────────────────────────────────────

function initSetup() {
  document.getElementById('setup-user').value = '';
  document.getElementById('setup-pass').value = '';
  document.getElementById('setup-pass2').value = '';
  document.getElementById('setup-error').textContent = '';
  document.getElementById('setup-pass2').onkeydown = e => { if (e.key === 'Enter') doSetup(); };
}

async function doSetup() {
  const user = document.getElementById('setup-user').value.trim();
  const pass = document.getElementById('setup-pass').value;
  const pass2 = document.getElementById('setup-pass2').value;
  const err = document.getElementById('setup-error');

  if (!user) { err.textContent = 'Username is required'; return; }
  if (pass.length < 4) { err.textContent = 'Password must be at least 4 characters'; return; }
  if (pass !== pass2) { err.textContent = 'Passwords do not match'; return; }

  const { ok, data } = await apiPost('/api/auth/setup', { username: user, password: pass });
  if (ok) {
    navigate('/dashboard');
  } else {
    err.textContent = data.error || 'Setup failed';
  }
}

