# Satellite — Design tokens (local web UI)

All theme colors for the Satellite local admin page live in
[web/style.css](web/style.css) as CSS custom properties on `:root`.

Token names follow the cross-repo schema documented in
`d:\TinkerNorth\BRAND.md` (TinkerNorth design system). When updating a
value, keep it in sync with the matching token in dish-android, dish-mac,
and dish-linux.

## Available tokens

### Core (matches cross-repo amber-family schema)

| Var | Value | Role |
|---|---|---|
| `--bg` | `#0D0F12` | Body / inputs / list-item background |
| `--surface` | `#161A1F` | Card / raised panel |
| `--outline` | `#222831` | Borders / dividers |
| `--primary` | `#FFC107` | Main accent (amber) |
| `--primary-mid` | `#C27A2C` | Secondary state (slider on, btn-stop hover) |
| `--primary-dark` | `#A65F1E` | Pressed / disabled primary (btn-stop bg, dot.off) |
| `--on-primary` | `#0D0F12` | Text/icon on top of primary |
| `--text` | `#EAEAEA` | Body text |
| `--text-muted` | `#6B7280` | Secondary text |
| `--success` | `#22C55E` | Status — success |
| `--warning` | `#FFC107` | Status — warning (aliases `--primary`) |
| `--error` | `#E74C3C` | Status — error |

### Satellite extensions (no cross-repo equivalent)

Bright/dark primary variants:
- `--primary-bright` `#FFDA3A` — hover state for `.btn-start`, `.btn-link`
- `--primary-gradient-end` `#FF9800` — offline progress bar gradient end

Extra muted greys (drift — see Outliers):
- `--text-muted-soft` `#B0B3B8` — form labels, toggle-row spans
- `--text-muted-cool` `#9CA3AF` — log filter labels

Offline / empty-state cooler text shades (drift):
- `--text-bright` `#F1F5F9` — `.offline-title`
- `--text-cool-bright` `#94A3B8` — `.offline-message`
- `--text-cool-dim` `#64748B` — `.offline-hint`

Error variants (drift):
- `--error-bright` `#EF4444` — `.ctrl-dot.err`
- `--error-light` `#F87171` — log error message text

Log viewer palette (semantic; satellite-only UI):
- `--bg-log` `#0A0C0F`
- `--outline-log` `#1E293B`
- `--log-row-hover` `#1a1f2b`
- `--log-info-bg` `#1E3A5F`, `--log-info-text` `#60A5FA`
- `--log-warn-bg` `#4A3728`, `--log-warn-text` `#FBBF24`
- `--log-error-bg` `#4A2020`, `--log-error-text` `#F87171`
- `--log-msg` `#D1D5DB`
- `--log-source` `#A78BFA`

Save-button palette (deep blue; nowhere else in the cross-repo schema):
- `--save-btn-bg` `#1E3A5F` (coincidentally same value as `--log-info-bg`)
- `--save-btn-hover` `#2F5D8C`

Misc:
- `--flow-arrow` `#333` — neutral divider tone for pipeline arrows

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

## Outliers

### 1. Drift inside the Satellite UI itself

The token block above exposes the drift that previously hid behind inline
hex literals. Several token families have **more than one shade for the
same role** — these are preserved because the user said "no value
changes," but they are candidates for consolidation in a future PR:

- **Muted grey**: 3 shades — `--text-muted` (`#6B7280`),
  `--text-muted-soft` (`#B0B3B8`), `--text-muted-cool` (`#9CA3AF`).
- **Error red**: 3 shades — `--error` (`#E74C3C`), `--error-bright`
  (`#EF4444`), `--error-light` (`#F87171`).
- **Background**: 2 shades — `--bg` (`#0D0F12`) and `--bg-log`
  (`#0A0C0F`); the log container intentionally goes slightly darker.
- **Outline**: 2 shades — `--outline` (`#222831`) and `--outline-log`
  (`#1E293B`).
- **Save-button blue** (`#1E3A5F` / `#2F5D8C`) is the only deep blue in
  the Satellite/Dish family — does not appear in any other repo.

### 2. Illustration assets (intentional, do not migrate)

- [web/img/ctrl-playstation.svg](web/img/ctrl-playstation.svg) — third-party
  brand colors: `#003087` (PlayStation blue), `#7BC8A4`, `#E8555D`,
  `#C8A2D4`, `#6B9FD4`, plus `#fff` overlays.
- [web/img/ctrl-xbox.svg](web/img/ctrl-xbox.svg) — `#107C10` (Xbox green)
  plus `#fff` overlays.
- [web/icon.png](web/icon.png) and [web/img/satellite-icon.png](web/img/satellite-icon.png)
  are byte-identical (`529,551 bytes` each). Future cleanup: consolidate
  to one canonical asset.

### 3. rgba derivatives (known follow-up)

The codebase has many `rgba(R, G, B, alpha)` literals derived from palette
colors — e.g. `rgba(255, 193, 7, ...)` is `--primary` at variable alpha,
and `rgba(255, 193, 7, 0.4)` appears in several glow / box-shadow rules.
These are not migrated in this pass because CSS does not allow
`rgba(var(...), ...)` without the relatively new
`rgb(from var(--x) r g b / alpha)` syntax. For now: if you change
`--primary`, audit any matching `rgba(255, 193, 7, ...)` literals by
hand.

### 4. Font choice (out of scope)

style.css imports `Rajdhani` from Google Fonts as the display font.
Other entities in the TinkerNorth family use Orbitron (web) or system
defaults (native). This font-family divergence is a separate cleanup
from the color token work.

## Recent migrations

- All inline color literals in [web/style.css](web/style.css) replaced with
  `var(--*)` references; the `:root` block is the new source of truth.
- [web/debug.js](web/debug.js) chart-bar color picker now reads
  `--success` / `--primary` / `--error` via `getComputedStyle()`.
