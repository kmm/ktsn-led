import type { Core, SimEnv } from './ktsnLed';

export const FRAME_HZ = 60;       // the boards render at 60 Hz (main/config.h FRAME_HZ)

/** Day of the year, 1..366, in local time (what `(env doy)` returns on a board). */
export function dayOfYear(d: Date): number {
  const start = new Date(d.getFullYear(), 0, 1);
  return Math.floor((+new Date(d.getFullYear(), d.getMonth(), d.getDate()) - +start) / 86400000) + 1;
}

export interface Location { lat: number; lon: number }
export type Sky = Pick<SimEnv, 'sunrise' | 'sunset' | 'dawn' | 'dusk' | 'moon' | 'moonlit' | 'daylight'>;
export const NO_SKY: Sky = { sunrise: -1, sunset: -1, dawn: -1, dusk: -1, moon: -1, moonlit: -1, daylight: -1 };

/** Unix seconds before 2023: a board treats its clock as not set (main/sky.h SKY_CLOCK_SET_AFTER). */
export const CLOCK_SET_AFTER = 1700000000;

const SUNRISE_ALT = -0.8333, CIVIL_ALT = -6;     // main/astro.h

/** A function giving what a board computes for a moment, recomputed only when the minute or the location changes,
 *  `daylight` each second (like main/sky.c). Times are the computer's local time, as the rest of the preview's clock. */
export function skyClock(core: Pick<Core, 'sunEvent' | 'moonAngle' | 'moonLit' | 'sunAltitude'>) {
  let key = '', sky = NO_SKY, dayKey = '', daylight = -1;
  return (when: Date, loc: Location | null): Sky => {
    const sec = Math.floor(+when / 1000);
    const dk = `${sec}|${loc?.lat}|${loc?.lon}`;
    if (dk !== dayKey) {
      dayKey = dk;
      daylight = loc && sec > CLOCK_SET_AFTER ? (core.sunAltitude(loc.lat, loc.lon, sec) > SUNRISE_ALT ? 1 : 0) : -1;
    }
    const k = `${Math.floor(+when / 60000)}|${loc?.lat}|${loc?.lon}`;
    if (k === key) return { ...sky, daylight };
    key = k;
    const t = Math.floor(+when / 1000);
    const from = +new Date(when.getFullYear(), when.getMonth(), when.getDate()) / 1000;
    const to = +new Date(when.getFullYear(), when.getMonth(), when.getDate() + 1) / 1000;
    const at = (alt: number, rising: boolean) => {
      if (!loc) return -1;
      const e = core.sunEvent(loc.lat, loc.lon, alt, rising, from, to);
      if (e === null) return -1;
      const d = new Date(e * 1000);
      return Math.min(1439, d.getHours() * 60 + d.getMinutes() + (d.getSeconds() >= 30 ? 1 : 0));
    };
    sky = {
      dawn: at(CIVIL_ALT, true), sunrise: at(SUNRISE_ALT, true), sunset: at(SUNRISE_ALT, false), dusk: at(CIVIL_ALT, false),
      moon: Math.round(core.moonAngle(t)) % 360, moonlit: Math.round(core.moonLit(t) * 100), daylight: -1,
    };
    return { ...sky, daylight };
  };
}

/** The env a board would see at `frame`, with the clock at `when` (local time, like a board with its TZ set). */
export function envAt(frame: number, when: Date, intensity: number, sky: Sky = NO_SKY): SimEnv {
  return {
    ...sky,
    frame,
    year: when.getFullYear(), month: when.getMonth() + 1, day: when.getDate(), doy: dayOfYear(when),
    hour: when.getHours(), minute: when.getMinutes(), second: when.getSeconds(), ms: when.getMilliseconds(),
    weekday: when.getDay(), timeset: +when / 1000 > CLOCK_SET_AFTER ? 1 : 0,
    intensity,
  };
}

/** Pixel 0xWWRRGGBB scaled by intensity (0..256) to a CSS colour; W is dropped like the RGB boards do. */
export function pixelCss(p: number, intensity: number): string {
  const k = Math.min(256, Math.max(0, intensity));
  const r = Math.round((((p >>> 16) & 255) * k) / 256), g = Math.round((((p >>> 8) & 255) * k) / 256), b = Math.round(((p & 255) * k) / 256);
  return `rgb(${r},${g},${b})`;
}

/** Screen response the "LED light" view assumes (sRGB is close to a 2.2 power curve). */
export const SCREEN_GAMMA = 2.2;

/** Output byte -> on-screen byte. With `ledLight`, the bytes a board sends (core.output: its gamma, intensity and
 *  dither) are taken as linear LED light and encoded for the screen, so the preview looks like the LEDs do: a board
 *  without gamma correction looks washed out on screen too, and gamma ~2.2 looks like the plain values. Without it,
 *  the bytes are drawn as they are. */
export function screenTable(ledLight: boolean): Uint8Array {
  const t = new Uint8Array(256);
  for (let b = 0; b < 256; b++) t[b] = ledLight ? Math.round(255 * Math.pow(b / 255, 1 / SCREEN_GAMMA)) : b;
  return t;
}

/** Pixel 0xWWRRGGBB through a screen table to a CSS colour (W dropped), or null if the LED would be dark. */
export function pixelCssVia(p: number, t: Uint8Array): string | null {
  const r = (p >>> 16) & 255, g = (p >>> 8) & 255, b = p & 255;
  return r | g | b ? `rgb(${t[r]},${t[g]},${t[b]})` : null;
}

/** What the eye sees of several sends within one frame (oversampled output): the per-byte average, rounded. The
 *  bytes are linear LED light, so averaging them is right; one send is returned as is. */
export function averageSends(sends: Uint32Array[]): Uint32Array {
  if (sends.length === 1) return sends[0];
  const n = sends.length, out = new Uint32Array(sends[0].length);
  for (let i = 0; i < out.length; i++) {
    let r = 0, g = 0, b = 0, w = 0;
    for (const s of sends) { const p = s[i]; w += p >>> 24; r += (p >>> 16) & 255; g += (p >>> 8) & 255; b += p & 255; }
    out[i] = ((Math.round(w / n) << 24) | (Math.round(r / n) << 16) | (Math.round(g / n) << 8) | Math.round(b / n)) >>> 0;
  }
  return out;
}

/** An output pixel (core.output: RGB as sent, W = white level) as screen colour: the W LED's light is the white point
 *  colour x ratio x level, added to RGB. With `ledLight` the sum is linear LED light encoded for the screen (like
 *  screenTable); otherwise drawn as is. null if the LED is dark. */
export function pixelCssWhite(p: number, point: number, ratio: number, ledLight: boolean): string | null {
  const wl = (p >>> 24) / 255;
  if (!(p & 0xffffff) && !wl) return null;
  const ch = (sh: number) => {
    const x = Math.min(1, ((p >>> sh) & 255) / 255 + wl * (((point >>> sh) & 255) / 255) * ratio);
    return Math.round(255 * (ledLight ? Math.pow(x, 1 / SCREEN_GAMMA) : x));
  };
  return `rgb(${ch(16)},${ch(8)},${ch(0)})`;
}
