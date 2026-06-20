// i18n.js: browser-side i18n for the satellite admin UI.
//
// Loads a JSON catalog from /lang/<locale>.json and exposes a synchronous
// `t(key, params)` lookup. No npm deps, no fetch-after-paint flash: callers
// that need translations during early script run should wait for the
// `i18n:ready` event (or just lean on the DOMContentLoaded walker, which
// runs after the catalog has loaded).
//
// Catalog format is a flat `{ key: "translated string" }` object. Keys are
// kebab-case grouped by surface: `dashboard.status.connected`,
// `modal.restart.title`, `pin.hint.expires`, etc. Placeholders use the
// Android style (`%1$s`, `%1$d`, `%2$s`) so the translation can be ported
// verbatim from dish-android/app/src/main/res/values-XX/strings.xml; we
// also accept `{name}` substitution for new web-only keys.
//
// Locale resolution:
//   1. ?lang=fr query-string override (testing)
//   2. navigator.language (and navigator.languages[]): match either the
//      full BCP-47 tag (pt-BR) or just the language subtag (pt)
//   3. fall back to 'en'

(function () {
  'use strict';

  const SHIPPED_LOCALES = ['en', 'bs', 'de', 'es', 'fr', 'pt-BR'];
  const FALLBACK_LOCALE = 'en';

  let catalog = {};      // active catalog (whatever loaded)
  let fallback = {};     // en catalog, used when a key is missing
  let activeLocale = FALLBACK_LOCALE;
  let ready = false;

  // Resolve a candidate BCP-47 tag onto a shipped locale, or null.
  function matchShipped(tag) {
    if (!tag) return null;
    // Direct match: en, bs, de, es, fr, pt-BR
    if (SHIPPED_LOCALES.indexOf(tag) !== -1) return tag;
    // Match the language subtag (e.g. "pt" → "pt-BR", "de-AT" → "de").
    const lang = tag.split('-')[0].toLowerCase();
    for (let i = 0; i < SHIPPED_LOCALES.length; i++) {
      const cand = SHIPPED_LOCALES[i];
      if (cand === lang) return cand;
      if (cand.toLowerCase().split('-')[0] === lang) return cand;
    }
    return null;
  }

  function detectLocale() {
    // 1) ?lang=xx override
    try {
      const qs = new URLSearchParams(window.location.search);
      const forced = qs.get('lang');
      if (forced) {
        const m = matchShipped(forced);
        if (m) return m;
      }
    } catch (e) { /* ignore */ }

    // 2) navigator.languages[] in priority order, then navigator.language
    const candidates = [];
    if (Array.isArray(navigator.languages)) {
      for (const l of navigator.languages) candidates.push(l);
    }
    if (navigator.language) candidates.push(navigator.language);
    for (const c of candidates) {
      const m = matchShipped(c);
      if (m) return m;
    }
    return FALLBACK_LOCALE;
  }

  // Fetch a catalog. Returns {} on any failure; the caller falls through to
  // the next locale or leaves keys verbatim.
  async function loadCatalog(locale) {
    try {
      const r = await fetch('/lang/' + locale + '.json', { cache: 'no-cache' });
      if (!r.ok) return {};
      return await r.json();
    } catch (e) {
      return {};
    }
  }

  // Interpolate Android-style (%1$s / %1$d / %%) and {name} placeholders.
  function interpolate(str, params) {
    if (!params) return str.replace(/%%/g, '%');
    let out = str;
    // {named} style
    out = out.replace(/\{(\w+)\}/g, function (_, k) {
      return (k in params) ? String(params[k]) : '{' + k + '}';
    });
    // %1$s / %1$d / %2$s: params can be an array OR positional args 1,2,3
    out = out.replace(/%(\d+)\$[sd]/g, function (_, idx) {
      const i = parseInt(idx, 10);
      if (Array.isArray(params)) {
        return (i - 1 < params.length) ? String(params[i - 1]) : '';
      }
      if (params && (i in params)) return String(params[i]);
      return '';
    });
    out = out.replace(/%%/g, '%');
    return out;
  }

  // Public lookup. Returns the translated string, falling back through
  // en → the raw key so a missing key is visible (rather than empty).
  function t(key, params) {
    let s = null;
    if (catalog && key in catalog) s = catalog[key];
    else if (fallback && key in fallback) s = fallback[key];
    if (s === null || s === undefined) return key;
    return interpolate(s, params);
  }

  // Walk the static DOM looking for data-i18n attributes and translate them.
  // Supports:
  //   <span data-i18n="foo.bar">English fallback text</span>
  //   <input data-i18n-attr="placeholder:foo.placeholder, title:foo.tip">
  function applyStaticDOM(root) {
    const scope = root || document;
    // Text content
    const textNodes = scope.querySelectorAll('[data-i18n]');
    for (let i = 0; i < textNodes.length; i++) {
      const el = textNodes[i];
      const key = el.getAttribute('data-i18n');
      if (!key) continue;
      el.textContent = t(key);
    }
    // Attributes
    const attrNodes = scope.querySelectorAll('[data-i18n-attr]');
    for (let i = 0; i < attrNodes.length; i++) {
      const el = attrNodes[i];
      const spec = el.getAttribute('data-i18n-attr');
      if (!spec) continue;
      const pairs = spec.split(',');
      for (let j = 0; j < pairs.length; j++) {
        const pair = pairs[j].trim();
        if (!pair) continue;
        const colon = pair.indexOf(':');
        if (colon < 0) continue;
        const attr = pair.slice(0, colon).trim();
        const key  = pair.slice(colon + 1).trim();
        if (!attr || !key) continue;
        el.setAttribute(attr, t(key));
      }
    }
  }

  // Kick off catalog load immediately; resolve before DOMContentLoaded when
  // possible so the static-DOM walker can run synchronously on first paint.
  const bootPromise = (async function boot() {
    activeLocale = detectLocale();
    document.documentElement.setAttribute('lang', activeLocale);
    // Always load English as the fallback first so missing keys still render.
    fallback = await loadCatalog(FALLBACK_LOCALE);
    if (activeLocale === FALLBACK_LOCALE) {
      catalog = fallback;
    } else {
      const loaded = await loadCatalog(activeLocale);
      // If the requested catalog 404'd, fall back to English rather than
      // showing raw keys.
      catalog = (loaded && Object.keys(loaded).length > 0) ? loaded : fallback;
    }
    ready = true;
    try { document.dispatchEvent(new CustomEvent('i18n:ready')); } catch (e) {}
  })();

  // Both events race; whichever happens last triggers the walker.
  function maybeWalk() {
    if (ready && document.readyState !== 'loading') applyStaticDOM(document);
  }
  document.addEventListener('DOMContentLoaded', maybeWalk);
  document.addEventListener('i18n:ready', maybeWalk);

  // Public surface
  window.t = t;
  window.i18n = {
    t: t,
    locale: function () { return activeLocale; },
    ready: function () { return bootPromise; },
    apply: applyStaticDOM,
  };
})();
