// Typed wrapper around the DSL core compiled to WebAssembly (ide/wasm). Same compiler and runtime as the boards, so
// compile errors and rendered pixels match a device exactly. No DOM or React here: usable from Electron or tests.
import createModule from '../wasm/generated/ktsn-led.mjs';
import type { AudioEnv } from './audio';

export const BUFFER_NAMES = ['X', 'Y', 'Z', 'O', 'G0', 'G1', 'G2', 'G3', 'G4', 'G5', 'G6', 'G7'] as const;
export type BufferName = (typeof BUFFER_NAMES)[number];

export interface NativeInfo { name: string; minArgs: number; maxArgs: number }

/** What a program sees through `(env ...)`. */
export interface SimEnv {
  frame: number;
  year: number; month: number; day: number; doy: number;
  hour: number; minute: number; second: number;
  ms: number;                     // 0..999 within the second
  weekday: number;                // 0..6, 0 = Sunday
  timeset: number;                // 1 once the clock is set (after 2023)
  intensity: number;              // 0..256
  sunrise: number; sunset: number; dawn: number; dusk: number;   // minutes after local midnight, -1 = none
  moon: number; moonlit: number;  // phase angle 0..359, percent lit
  daylight: number;               // 1 sun up, 0 down, -1 = no location
}

export interface CompileError { line: number; col: number; message: string; text: string }

/** "line L col C: message" from the compiler. Columns count bytes, like the boards do. */
export function parseError(text: string): CompileError | null {
  if (!text) return null;
  const m = /^line (\d+) col (\d+): (.*)$/s.exec(text);
  return m ? { line: +m[1], col: +m[2], message: m[3], text } : { line: 1, col: 1, message: text, text };
}

/** A channel's wire format and white handling (board config: wire, wmode, wpoint, wratio, wmax). */
export interface WhiteSettings { wire: string; mode: 'direct' | 'extract' | 'boost'; point: number; ratio: number; max: number }
export const DEFAULT_WHITE: WhiteSettings = { wire: 'RGB', mode: 'direct', point: 0xffb46b, ratio: 1, max: 100 };
const WMODES = ['direct', 'extract', 'boost'] as const;

export type DslContext = 'render' | 'automation';
export type UseSources = Map<string, string | { why: string }>;

export interface Core {
  /** (Re)create the simulated channel; the 8 global buffers keep their contents. */
  init(npix: number): void;
  /** Compile only. null if fine. `context` 'automation' checks it as the automation program (G buffers only, the
   *  automation functions allowed); the running context is left as it was. */
  check(src: string, context?: DslContext): CompileError | null;
  /** What load() compiles for from now on (default 'render'). */
  setContext(context: DslContext): void;
  /** Library names (play ...) is checked against, e.g. the connected board's; null accepts any name. */
  setLibrary(names: string[] | null): void;
  /** The names last given to setLibrary (null when offline). */
  libraryNames(): string[] | null;
  /** What (use name) sees: name -> source, or {why} when the name can't be used (the compile error). Replaces the set. */
  setUseSources(sources: UseSources): void;
  useSources(): UseSources;
  /** check() against other (use name) sources; the current ones are put back. */
  checkUsing(src: string, sources: UseSources, context?: DslContext): CompileError | null;
  /** The connected board's GPIO outputs (io ...) is checked against: slot numbers with optional names; null = no
   *  board (any slot 0..7 and any name are accepted). */
  /** The connected board's GPIO slots (outputs and inputs, mode as the board names it), or null: no board, any name. */
  setOutputs(outputs: { n: number; name: string; mode?: string }[] | null): void;
  /** What an input reads in the preview, by slot number or name: a digital input true/false, an analog one 0..1. */
  setInput(slot: number | string, value: boolean | number): void;
  /** What (env band0..band7 / level / beats / beatms) read in the preview; null = no audio input (0, beatms 2147483647). */
  setAudio(a: AudioEnv | null): void;
  /** Compile and run it from the next frame; on error the old program keeps running. */
  load(src: string): CompileError | null;
  /** Run once in a temporary channel, like a board's autoexec. Returns an error/fault message or "". */
  autoexec(src: string): string;
  render(env: SimEnv): number;    // returns nodes evaluated
  /** Unix seconds of the first such event in [from, to), or null. */
  sunEvent(lat: number, lon: number, altitude: number, rising: boolean, from: number, to: number): number | null;
  moonAngle(t: number): number;   // 0 <= angle < 360
  moonLit(t: number): number;     // 0..1
  sunAltitude(lat: number, lon: number, t: number): number;   // degrees, unix seconds
  faults(): number;
  lastError(): string;
  scratchHigh(): number;
  seed(n: number): void;
  clear(): void;
  /** A copy of a buffer's pixels (0xWWRRGGBB). */
  buffer(name: BufferName): Uint32Array;
  natives(): NativeInfo[];
  /** The board's gamma table (main/gamma.c, byte-identical): gamma 1.0..3.0, 1 = off (identity). */
  gammaLut(gamma: number): Uint8Array;
  /** Buffer O through the board's output stage (main/dither.h, main/wire.h): gamma, white mode, intensity (0..256, fractions
   *  allowed: the board's output uses 1/256 of that, e.g. mid-fade or global x channel), white limit, dither for this frame, W spread over the wire's W outputs. 0xWWRRGGBB: RGB as sent, W = white level
   *  0..255 (0 if the wire has no W). */
  output(gamma: number, intensity: number, dither: number, frame: number, white?: WhiteSettings): Uint32Array;
}

let corePromise: Promise<Core> | null = null;

/** Load the module once; every caller shares it. */
export function loadCore(): Promise<Core> {
  corePromise ??= createModule().then((M) => {
    const fn = (name: string, ret: 'number' | null, args: ('number' | 'string')[]) => M.cwrap(name, ret, args);
    const str = (name: string, args: ('number' | 'string')[]) => {
      const f = M.cwrap(name, 'number', args);
      return (...a: (number | string)[]) => M.UTF8ToString(f(...a));
    };
    const init = fn('lc_init', 'number', ['number']);
    const check = str('lc_check', ['string']);
    const setCtx = fn('lc_set_context', null, ['number']);
    const setLib = fn('lc_set_library', null, ['string']);
    let libNames: string[] | null = null;
    const setIo = fn('lc_set_io', null, ['string']);
    const setIn = fn('lc_set_input', null, ['number', 'string', 'number']);
    const setAud = fn('lc_set_audio', null, Array(11).fill('number'));
    const useClear = fn('lc_use_clear', null, []);
    const useAdd = fn('lc_use_add', null, ['string', 'string', 'string']);
    let uses: UseSources = new Map();
    const pushUses = (m: UseSources) => {
      useClear();
      for (const [name, v] of m) useAdd(name, typeof v === 'string' ? v : '', typeof v === 'string' ? '' : v.why);
    };
    let ctx: DslContext = 'render';
    const ctxNum = (c: DslContext) => (c === 'automation' ? 1 : 0);
    const load = str('lc_load', ['string']);
    const autoexec = str('lc_autoexec', ['string']);
    const render = fn('lc_render', 'number', []);
    const faults = fn('lc_faults', 'number', []);
    const lastError = str('lc_last_error', []);
    const scratchHigh = fn('lc_scratch_high', 'number', []);
    const seed = fn('lc_seed', null, ['number']);
    const clear = fn('lc_clear', null, []);
    const buf = fn('lc_buf', 'number', ['number']);
    const bufLen = fn('lc_buf_len', 'number', ['number']);
    const setEnv = fn('lc_set_env', null, Array(19).fill('number'));
    const sunAltitude = fn('lc_sun_altitude', 'number', Array(3).fill('number'));
    const sunEvent = fn('lc_sun_event', 'number', Array(6).fill('number'));
    const moonAngle = fn('lc_moon_angle', 'number', ['number']);
    const moonLit = fn('lc_moon_lit', 'number', ['number']);
    const nCount = fn('lc_native_count', 'number', []);
    const nName = str('lc_native_name', ['number']);
    const nMin = fn('lc_native_min_args', 'number', ['number']);
    const nMax = fn('lc_native_max_args', 'number', ['number']);
    const gLut = fn('lc_gamma_lut', 'number', ['number']);
    const out = fn('lc_output', 'number', ['number', 'number', 'number', 'number', 'string', 'number', 'number', 'number', 'number']);

    const core: Core = {
      init(npix) { if (init(npix) !== 0) throw new Error(`could not create a ${npix}-pixel channel`); },
      check(src, c = 'render') {
        if (c !== ctx) setCtx(ctxNum(c));
        const e = parseError(check(src));
        if (c !== ctx) setCtx(ctxNum(ctx));
        return e;
      },
      setContext(c) { ctx = c; setCtx(ctxNum(c)); },
      setLibrary(names) { libNames = names; setLib(names === null ? '-' : names.join('\n')); },
      libraryNames: () => libNames,
      setUseSources(m) { uses = m; pushUses(m); },
      useSources: () => uses,
      checkUsing(src, m, c = 'render') {
        pushUses(m);
        const e = core.check(src, c);
        pushUses(uses);
        return e;
      },
      setOutputs(o) { setIo(o === null ? '-' : o.map((x) => `${x.n}:${x.name}${x.mode ? `:${x.mode}` : ''}`).join('\n')); },
      setInput(slot, value) {
        const v = typeof value === 'boolean' ? (value ? 1 : 0) : Math.round(Math.min(1, Math.max(0, value)) * 65536);
        setIn(typeof slot === 'number' ? slot : -1, typeof slot === 'string' ? slot : '', v);
      },
      setAudio(a) {
        const q = (v: number) => Math.round(Math.min(1, Math.max(0, v)) * 65536);
        if (!a) setAud(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -1);
        else setAud(...a.band.slice(0, 8).map(q), q(a.level), Math.max(0, Math.floor(a.beats)), Math.floor(a.beatms));
      },
      load: (src) => parseError(load(src)),
      autoexec: (src) => autoexec(src),
      sunEvent(lat, lon, alt, rising, from, to) { const t = sunEvent(lat, lon, alt, rising ? 1 : 0, from, to) as number; return t < 0 ? null : t; },
      moonAngle: (t) => moonAngle(t) as number,
      moonLit: (t) => moonLit(t) as number,
      sunAltitude: (lat, lon, t) => sunAltitude(lat, lon, t) as number,
      render(e) {
        setEnv(e.frame >>> 0, e.year, e.month, e.day, e.doy, e.hour, e.minute, e.second, e.intensity,
          e.sunrise, e.sunset, e.dawn, e.dusk, e.moon, e.moonlit, e.ms, e.weekday, e.timeset, e.daylight);
        return render();
      },
      faults: () => faults(),
      lastError: () => lastError(),
      scratchHigh: () => scratchHigh(),
      seed: (n) => { seed(n >>> 0); },
      clear: () => { clear(); },
      buffer(name) {
        const id = BUFFER_NAMES.indexOf(name);
        const p = buf(id) >>> 2, n = bufLen(id);
        return M.HEAPU32.slice(p, p + n);            // HEAPU32 is re-read each time: memory may have grown
      },
      gammaLut(g) {
        const p = gLut(Math.round(g * 100));
        return M.HEAPU8.slice(p, p + 256);
      },
      output(g, k, d, frame, w = DEFAULT_WHITE) {
        const p = out(Math.round(g * 100), Math.round(k * 256), d, frame >>> 0, w.wire, WMODES.indexOf(w.mode), w.point >>> 0,
          Math.round(w.ratio * 100), Math.round(w.max)) >>> 2, n = bufLen(BUFFER_NAMES.indexOf('O'));
        return M.HEAPU32.slice(p, p + n);
      },
      natives() {
        const out: NativeInfo[] = [];
        for (let i = 0; i < nCount(); i++) out.push({ name: nName(i), minArgs: nMin(i), maxArgs: nMax(i) });
        return out;
      },
    };
    return core;
  });
  return corePromise;
}
