function initDonate() {
  if (typeof window.t !== 'function') return;
  const nodes = document.querySelectorAll('#view-donate [data-i18n-html]');
  nodes.forEach(el => {
    const key = el.getAttribute('data-i18n-html');
    if (key) el.innerHTML = window.t(key);
  });
}
