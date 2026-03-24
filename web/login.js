// ── login.js — Login page ───────────────────────────────────────────────────

function initLogin() {
  // Clear fields every time login page is shown (after logout, etc.)
  document.getElementById('login-user').value = '';
  document.getElementById('login-pass').value = '';
  document.getElementById('login-error').textContent = '';
  document.getElementById('login-pass').onkeydown = e => { if (e.key === 'Enter') doLogin(); };
}

async function doLogin() {
  const user = document.getElementById('login-user').value.trim();
  const pass = document.getElementById('login-pass').value;
  const err = document.getElementById('login-error');

  const { ok, data } = await apiPost('/api/auth/login', { username: user, password: pass });
  if (ok) {
    err.textContent = '';
    navigate('/dashboard');
  } else {
    err.textContent = data.error || 'Login failed';
  }
}

