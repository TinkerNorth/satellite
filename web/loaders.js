// Loaders ported from the design spec (app-icon/project/app-essentials.jsx).
// SMIL is used directly so there's no animation library or RAF loop. Each
// function returns an HTML string for innerHTML / template interpolation.

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
