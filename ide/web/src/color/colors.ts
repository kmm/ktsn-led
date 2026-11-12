// Colours as the DSL writes them: #RRGGBB or #RRGGBBWW (W = the white byte), and the IDE's favourite colours.
import { useEffect, useState } from 'react';

export interface Rgbw { r: number; g: number; b: number; w: number }

export const HEX_RE = /^#([0-9a-fA-F]{6})([0-9a-fA-F]{2})?$/;

/** "#FF8000" / "#FF800040" -> components; null if it isn't one. */
export function parseHex(s: string): Rgbw | null {
  const m = HEX_RE.exec(s.trim());
  if (!m) return null;
  const v = parseInt(m[1], 16);
  return { r: (v >> 16) & 255, g: (v >> 8) & 255, b: v & 255, w: m[2] ? parseInt(m[2], 16) : 0 };
}

/** Components -> "#RRGGBB", or "#RRGGBBWW" when W is set or `withW` (keep the 8-digit form an edited literal had). */
export function formatHex(c: Rgbw, withW = false): string {
  const h = (n: number) => Math.max(0, Math.min(255, Math.round(n))).toString(16).padStart(2, '0').toUpperCase();
  return `#${h(c.r)}${h(c.g)}${h(c.b)}${withW || c.w ? h(c.w) : ''}`;
}

/** CSS for a swatch: RGB, with W shown as white mixed in (a rough look; the preview models LEDs properly). */
export function swatchCss(c: Rgbw): string {
  const k = c.w / 255;
  const mix = (v: number) => Math.round(v + (255 - v) * k * 0.6);
  return `rgb(${mix(c.r)},${mix(c.g)},${mix(c.b)})`;
}

/* ---------------- favourites: 16 slots, shared by every colour editor in the IDE ---------------- */

export const FAVORITE_SLOTS = 16;
const KEY = 'ktsn-led.ide.favorites';

export function loadFavorites(storage: Pick<Storage, 'getItem'> = localStorage): (string | null)[] {
  let v: unknown = null;
  try { v = JSON.parse(storage.getItem(KEY) ?? 'null'); } catch { /* fresh */ }
  const list = Array.isArray(v) ? v : [];
  return Array.from({ length: FAVORITE_SLOTS }, (_, i) => (typeof list[i] === 'string' && parseHex(list[i]) ? list[i] as string : null));
}

const listeners = new Set<(f: (string | null)[]) => void>();

export function saveFavorites(f: (string | null)[], storage: Pick<Storage, 'setItem'> = localStorage) {
  try { storage.setItem(KEY, JSON.stringify(f)); } catch { /* not remembered */ }
  for (const l of listeners) l(f);
}

/** Put a colour in the first free slot (or the given one). Returns the new list; unchanged if it is already there or full. */
export function addFavorite(f: (string | null)[], hex: string, slot?: number): (string | null)[] {
  if (f.includes(hex) && slot === undefined) return f;
  const i = slot ?? f.indexOf(null);
  if (i < 0 || i >= FAVORITE_SLOTS) return f;
  const n = [...f];
  n[i] = hex;
  return n;
}

/** The favourites, kept in sync across every editor (and other tabs of the IDE). */
export function useFavorites(): [(string | null)[], (f: (string | null)[]) => void] {
  const [fav, setFav] = useState(loadFavorites);
  useEffect(() => {
    const l = (f: (string | null)[]) => setFav(f);
    const onStorage = (e: StorageEvent) => { if (e.key === KEY) setFav(loadFavorites()); };
    listeners.add(l);
    window.addEventListener('storage', onStorage);
    return () => { listeners.delete(l); window.removeEventListener('storage', onStorage); };
  }, []);
  return [fav, saveFavorites];
}
