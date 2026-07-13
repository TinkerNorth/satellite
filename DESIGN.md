# Satellite design tokens (local web UI)

All theme colors for the Satellite local admin page live in
[web/style.css](web/style.css) as CSS custom properties on `:root`.

Token names follow the cross-repo schema documented in
`d:\TinkerNorth\BRAND.md` (TinkerNorth design system). When updating a
value, keep it in sync with the matching token in dish-android, dish-mac,
and dish-linux.

## Core tokens (cross-repo schema)

These 12 names are the contract shared with every other project in the
TinkerNorth family.

Palette: cyan / deep-space, mirroring dish-website.

| Var | Value | Role |
|---|---|---|
| `--bg` | `#060818` | Body background (`--tn-ink`) |
| `--surface` | `#0C1027` | Card / raised panel (`--tn-night`) |
| `--surface-dim` | `#131A3A` | Recessed / empty state (`--tn-deep`) |
| `--outline` | `rgba(79, 227, 255, 0.18)` | Borders / dividers (cyan @ ~18% alpha) |
| `--primary` | `#4FE3FF` | Main accent, cyan (`--tn-signal`) |
| `--primary-mid` | `#2C93AD` | Aliased to `--primary-dim` (web has no mid) |
| `--primary-dim` | `#2C93AD` | Pressed / disabled (`--tn-signal-dim`) |
| `--on-primary` | `#060818` | Text/icon on top of primary |
| `--text` | `#E6ECFF` | Body text (`--body-color`) |
| `--text-muted` | `#93A0C8` | Secondary text (`--muted`) |
| `--success` | `#22C55E` | Status: success |
| `--warning` | `#F59E0B` | Status: warning |
| `--error` | `#E74C3C` | Status: error |

## Typography + shape (dish-android voice)

The visual language mirrors dish-android's Material 3 voice: sans-serif
titles/body/buttons in normal case, a small monospace letter-spaced
eyebrow for section labels and the brand wordmark, no neon glow, and a
graduated corner-radius scale. Only the cyan/deep-space palette above is shared
with the old "cyberpunk console" look.

| Var | Value | Role |
|---|---|---|
| `--font-sans` | `'Inter', system-ui, …` | Titles, body, names, buttons (normal case) |
| `--font-mono` | `ui-monospace, 'Source Code Pro', …` | Eyebrows, brand wordmark, values, IPs, code |
| `--corner-chip` | `4px` | Status chips, small badges |
| `--corner-button` | `6px` | Buttons, inputs |
| `--corner-card` | `8px` | Cards, list rows, log container |
| `--corner-notification` | `10px` | Notices, banners, modal |
| `--corner-pill` | `20px` | Switch track, count badges |

Eyebrows (`.section-title`) and the brand wordmark (`h1`) are the only
uppercase, letter-spaced (`0.12em`) elements; everything else is sentence or
title case. Buttons follow dish-android's two-tier system: filled
(`--primary-dim` bg, `--on-primary` text) and outlined (transparent +
`--outline` border).

## Log-viewer extension (scoped, satellite-only)

The log viewer has its own paired bg+text palette for the three log
severities. This is a separate semantic surface from the core status
colors (`--success` / `--warning` / `--error` are for general UI; these
are specifically for `.log-badge` and `.log-line` selectors).

| Var | Value | Role |
|---|---|---|
| `--log-info-bg` | `#1E3A5F` | Info badge bg |
| `--log-info-text` | `#60A5FA` | Info badge text |
| `--log-warn-bg` | `#4A3728` | Warn badge bg |
| `--log-warn-text` | `#FBBF24` | Warn badge text |
| `--log-error-bg` | `#4A2020` | Error badge bg |
| `--log-error-text` | `#F87171` | Error badge text (also used for `.log-line.log-error .log-msg`) |
| `--log-msg` | `#D1D5DB` | Default log message text |
| `--log-source` | `#A78BFA` | Log source prefix |

## How to use

In `style.css`:
```css
.my-card {
  background: var(--surface);
  color: var(--text);
  border: 1px solid var(--outline);
}
```

In `debug.js` (and other JS files that need theme colors):
```js
function themeColor(name) {
  return getComputedStyle(document.documentElement).getPropertyValue(name).trim();
}
const accent = themeColor('--primary');
```

## Outliers: illustration assets (intentional, never theme tokens)

The controller illustrations carry their own baked-in fills and are not
driven by the theme tokens above. Leave them alone when retheming.

- [web/img/ctrl-playstation.svg](web/img/ctrl-playstation.svg)
- [web/img/ctrl-xbox.svg](web/img/ctrl-xbox.svg)

## Known follow-ups (out of scope for this PR)

1. **Asset duplication**: [web/icon.png](web/icon.png) and
   [web/img/satellite-icon.png](web/img/satellite-icon.png) are
   byte-identical duplicates. Consolidate to one canonical asset.
2. **rgba derivatives**: cyan/amber/red literals at alpha (e.g.
   `rgba(79, 227, 255, 0.14)`) appear in chip/notice background rules.
   Migration deferred until repo-wide adoption of
   `rgb(from var(--x) r g b / alpha)` syntax. If a base token changes,
   audit those literals by hand.
3. **Translation pass**: the dish-android alignment added
   `dashboard.region.pairing` and `debug.stats.http-port` to `lang/en.json`
   only; the other locales fall back to English until a translator fills them.

## Consolidation history

This file previously defined 34 tokens. The following 15 were deleted in
favor of the 12-token core schema:

| Removed | Replaced with |
|---|---|
| `--text-muted-soft` `#B0B3B8` | `--text-muted` `#6B7280` |
| `--text-muted-cool` `#9CA3AF` | `--text-muted` `#6B7280` |
| `--text-bright` `#F1F5F9` | `--text` `#EAEAEA` |
| `--text-cool-bright` `#94A3B8` | `--text-muted` `#6B7280` |
| `--text-cool-dim` `#64748B` | `--text-muted` `#6B7280` |
| `--error-bright` `#EF4444` | `--error` `#E74C3C` |
| `--error-light` `#F87171` | `--log-error-text` (same hex; scope corrected) |
| `--bg-log` `#0A0C0F` | `--bg` `#0D0F12` |
| `--outline-log` `#1E293B` | `--outline` `#222831` |
| `--log-row-hover` `#1a1f2b` | `--surface` `#161A1F` |
| `--save-btn-bg` `#1E3A5F` | `--primary-dim` `#A65F1E` |
| `--save-btn-hover` `#2F5D8C` | `--primary-mid` `#C27A2C` |
| `--primary-bright` `#FFDA3A` | `--primary` `#FFC107` |
| `--primary-gradient-end` `#FF9800` | `--primary-dim` `#A65F1E` |
| `--flow-arrow` `#333` | `--outline` `#222831` |

Additionally:
- `--primary-dark` renamed to `--primary-dim` to align with the schema.
- `--warning` value changed from `#FFC107` (aliased primary) to `#F59E0B`,
  matching dish-android / dish-mac / dish-linux. Warning-context selectors
  (`.backend-dot.backend-warn`, `.backend-warn-text`, `.flow-step.warn`,
  `.pipe-stage.pipe-idle`, `.debug-warn`) updated to reference
  `var(--warning)` instead of `var(--primary)`.

This is a visible change: the orange-amber warning is now distinct from
the yellow-amber primary. Previously, warning states were indistinguishable
from primary.
