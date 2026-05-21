// ── loaders.js — Three indeterminate progress indicators ─────────────────────
//
// Ported verbatim from the design spec in
// `app-icon/project/app-essentials.jsx` (sections `LoaderSpinner`,
// `LoaderDots`, `LoaderBar`). Proportions (stroke width, dasharray ratio,
// dot radius, slider width, timings) are pixel-faithful to the spec; the
// color is the satellite brand's `--primary` rather than the spec's
// hard-coded `#8FCFE3` so loaders match the rest of the dashboard.
//
// SMIL (`<animate>` / `<animateTransform>`) is used directly — that's what
// the spec ships and modern browsers parse inline SVG SMIL natively. No
// animation library, no JS-driven RAF loop.
//
// Each function returns an HTML string so it can be dropped into a button
// or container via `innerHTML +=` / template-literal interpolation. Callers
// that need a Node can wrap the string with `document.createRange()
// .createContextualFragment(html).firstElementChild`.
//
// Use:
//   · spinnerSVG() — default. Short, bounded waits (network calls, scans).
//   · dotsSVG()    — "thinking" states (not used on the dashboard today).
//   · barSVG()     — whole-pane / area-level loading (SSE reconnect bar).

// Indeterminate spinner — partial arc rotating.
// 64×64 design canvas: stroke 6u, dasharray "50 88" → arc covers ~36% of the
// ring. 1.2s linear rotation.
function spinnerSVG(size) {
  const s = (typeof size === 'number') ? size : 16;
  return (
    '<svg class="ldr-spinner" viewBox="0 0 64 64" width="' + s + '" height="' + s + '" aria-label="Loading" role="img" style="display:block">' +
      '<circle cx="32" cy="32" r="22" fill="none" stroke="currentColor" stroke-width="6" opacity="0.25"/>' +
      '<g style="transform-origin:32px 32px">' +
        '<circle cx="32" cy="32" r="22" fill="none" stroke="currentColor" stroke-width="6" stroke-linecap="round" stroke-dasharray="50 88">' +
          '<animateTransform attributeName="transform" type="rotate" from="0 32 32" to="360 32 32" dur="1.2s" repeatCount="indefinite"/>' +
        '</circle>' +
      '</g>' +
    '</svg>'
  );
}

// Three pulsing dots — opacity 0.25↔1 and r 4↔6 on a 1.2s cycle, staggered
// by 0.18s per dot. Kept for parity with the spec; the dashboard does not
// surface a "thinking" state today, but the loader is exported so any
// future caller can use it without re-implementing the SMIL.
function dotsSVG(size) {
  const s = (typeof size === 'number') ? size : 16;
  let dots = '';
  [16, 32, 48].forEach((cx, i) => {
    const begin = (i * 0.18) + 's';
    dots +=
      '<circle cx="' + cx + '" cy="32" r="5" fill="currentColor">' +
        '<animate attributeName="opacity" values="0.25;1;0.25" dur="1.2s" begin="' + begin + '" repeatCount="indefinite"/>' +
        '<animate attributeName="r"       values="4;6;4"       dur="1.2s" begin="' + begin + '" repeatCount="indefinite"/>' +
      '</circle>';
  });
  return (
    '<svg class="ldr-dots" viewBox="0 0 64 64" width="' + s + '" height="' + s + '" aria-label="Working" role="img" style="display:block">' +
      dots +
    '</svg>'
  );
}

// Indeterminate horizontal bar — 80u-wide slider on a 240u track, 1.4s
// linear cycle (`x` runs -80 → 240 so the slider enters and exits cleanly).
// Track opacity 0.22 to match the spec.
function barSVG(width) {
  const w = (typeof width === 'number') ? width : 240;
  const h = w * (16 / 240);
  return (
    '<svg class="ldr-bar" viewBox="0 0 240 16" width="' + w + '" height="' + h + '" preserveAspectRatio="none" aria-label="Loading" role="img" style="display:block">' +
      '<rect x="0"   y="4" width="240" height="8" rx="4" fill="currentColor" opacity="0.22"/>' +
      '<rect x="-80" y="4" width="80"  height="8" rx="4" fill="currentColor">' +
        '<animate attributeName="x" values="-80;240" dur="1.4s" repeatCount="indefinite"/>' +
      '</rect>' +
    '</svg>'
  );
}
