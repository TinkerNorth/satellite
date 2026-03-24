async function poll() {
  try {
    const r = await fetch('/api/status');
    const d = await r.json();

    document.getElementById('s-status').textContent = d.listening ? 'Listening' : 'Stopped';
    document.getElementById('s-packets').textContent = d.packets.toLocaleString();
    document.getElementById('s-sender').textContent = d.senderIP;
    document.getElementById('s-port').textContent = d.udpPort;

    const dot = document.getElementById('dot');
    dot.className = 'dot ' + (d.listening ? 'on' : 'off');

    const btn = document.getElementById('btnToggle');
    btn.textContent = d.listening ? 'Stop' : 'Start';
    btn.className = 'btn ' + (d.listening ? 'btn-stop' : 'btn-start');

    document.getElementById('udpPort').value = d.udpPort;
    document.getElementById('autoStart').checked = d.autoStart;
  } catch (e) {
    // Server unreachable — ignore
  }
}

async function toggle() {
  const r = await fetch('/api/status');
  const d = await r.json();
  await fetch(d.listening ? '/api/stop' : '/api/start', { method: 'POST' });
  setTimeout(poll, 300);
}

async function saveConfig() {
  const body = JSON.stringify({
    udpPort: parseInt(document.getElementById('udpPort').value),
    autoStart: document.getElementById('autoStart').checked
  });
  await fetch('/api/config', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body
  });
  poll();
}

poll();
setInterval(poll, 1000);

