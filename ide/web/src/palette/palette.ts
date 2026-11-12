// Palettes: colour stops stored in a G buffer (G0..G7) with `set`, used by fill-grad / fill-pat / grad-map.
// Pure helpers: the board's gradient and pattern maths (for the preview), the code that writes a palette, saved palettes.
import { formatHex, parseHex, type Rgbw } from '../color/colors';

export const MAX_STOPS = 32;
export const SET_MAX = 8;                     // colours per (set ...) call (the DSL's limit)

/** px_mix (components/dsl/include/pixel.h) per byte: (a*(256-k) + b*k + 128) >> 8. */
function mix(a: Rgbw, b: Rgbw, k: number): Rgbw {
  const m = (x: number, y: number) => (x * (256 - k) + y * k + 128) >> 8;
  return { r: m(a.r, b.r), g: m(a.g, b.g), b: m(a.b, b.b), w: m(a.w, b.w) };
}

/** What (fill-grad buf palette) puts in `n` pixels: stops spread evenly, first and last exactly at the ends,
 *  linear blend in between with the board's rounding (natives.c n_fill_grad). */
export function gradient(stops: Rgbw[], n: number): Rgbw[] {
  if (!stops.length || n <= 0) return [];
  const den = n - 1, step = stops.length - 1;
  const out: Rgbw[] = [];
  for (let i = 0; i < n; i++) {
    const num = i * step, seg = den ? Math.floor(num / den) : 0, rem = den ? num % den : 0;
    out.push(rem ? mix(stops[seg], stops[seg + 1], Math.floor((rem * 256 + Math.floor(den / 2)) / den)) : stops[seg]);
  }
  return out;
}

/** What (fill-pat buf palette 0 stretch) puts in `n` pixels: the stops repeated, each `stretch` pixels wide. */
export function pattern(stops: Rgbw[], n: number, stretch = 1): Rgbw[] {
  if (!stops.length || n <= 0) return [];
  return Array.from({ length: n }, (_, i) => stops[Math.floor(i / Math.max(1, stretch)) % stops.length]);
}

export interface PaletteTarget { buf: number; start: number; name: string }   // G0..G7, first index, def name ("" = none)

/** A DSL variable name, as (def ...) accepts it. */
export const VAR_NAME = /^[a-zA-Z][a-zA-Z0-9_-]{0,14}$/;

/** The code that stores the palette: one (set ...) per 8 colours on a slice of the G buffer; with a name, a (def ...)
 *  so programs can use it as `name`. Colours keep the 8-digit form only where W is set. */
export function paletteCode(stops: string[], t: PaletteTarget): string {
  const n = stops.length;
  if (!n) return '';
  const slice = `(slice (buf G${t.buf}) ${t.start} ${t.start + n})`;
  const hex = stops.map((s) => formatHex(parseHex(s) ?? { r: 0, g: 0, b: 0, w: 0 }));
  const sets: string[] = [];
  for (let i = 0; i < n; i += SET_MAX) sets.push(`${i} ${hex.slice(i, i + SET_MAX).join(' ')}`);
  const name = VAR_NAME.test(t.name) && !['t', 'nil', '_'].includes(t.name) ? t.name : '';
  if (sets.length === 1) return name ? `(def ${name} (set ${slice} ${sets[0]}))\n` : `(set ${slice} ${sets[0]})\n`;
  const target = name || slice;
  return (name ? `(def ${name} ${slice})\n` : '') + sets.map((s) => `(set ${target} ${s})\n`).join('');
}

/* ---------------- saved palettes (browser storage) ---------------- */

export interface SavedPalette { name: string; stops: string[] }
const KEY = 'ktsn-led.ide.palettes';

export function loadPalettes(storage: Pick<Storage, 'getItem'> = localStorage): SavedPalette[] {
  try {
    const v = JSON.parse(storage.getItem(KEY) ?? '[]');
    return Array.isArray(v) ? v.filter((p) => typeof p?.name === 'string' && Array.isArray(p.stops) && p.stops.every((s: unknown) => typeof s === 'string' && parseHex(s))) : [];
  } catch { return []; }
}

export function savePalettes(list: SavedPalette[], storage: Pick<Storage, 'setItem'> = localStorage) {
  try { storage.setItem(KEY, JSON.stringify(list)); } catch { /* not remembered */ }
}
