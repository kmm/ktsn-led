// The preview's stand-in for a board's audio input: a fake beat at a chosen tempo, so programs using
// (env band0..band7), level, beats and beatms move without a microphone. Pure and deterministic (a function of the
// preview's own time), like the board updated once per 32 ms block for bands and level; beatms follows every frame.

export interface AudioEnv {
  band: number[];                 // 8 values 0..1, low to high
  level: number;                  // 0..1
  beats: number;
  beatms: number;                 // ms since the latest beat
}

export const AUDIO_BLOCK_MS = 32;
export const BPM_MIN = 40, BPM_MAX = 240, BPM_DEFAULT = 120;

const AUDIO_ENV = /\(\s*env\s+(band[0-7]|level|beats|beatms)(?=[\s)])/;

/** Whether a program reads any audio env value (comments and strings skipped). */
export function usesAudio(src: string): boolean {
  return AUDIO_ENV.test(src.replace(/"(?:[^"\\\n]|\\.)*"/g, '""').replace(/;[^\n]*/g, ''));
}

/** The fake beat `tMs` after the preview started: a kick on every beat (bass jumps, then falls in ~0.2 s), hats
 *  between beats (the top bands), slowly moving mids. beats counts the kicks so far, beatms is since the latest. */
export function fakeAudio(tMs: number, bpm: number): AudioEnv {
  const t = Math.max(0, tMs);
  const period = 60000 / Math.min(BPM_MAX, Math.max(BPM_MIN, bpm));
  const k = Math.floor(t / period);
  const tb = Math.floor(t / AUDIO_BLOCK_MS) * AUDIO_BLOCK_MS;          // bands change once per block
  const end = tb + AUDIO_BLOCK_MS;                                     // a block shows the beat that started in it
  const sinceB = end - Math.floor(end / period) * period;
  const kick = Math.exp(-sinceB / 120);
  const hat = Math.exp(-(((sinceB - period / 2) % period + period) % period) / 60);
  const wob = (b: number) => 0.5 + 0.5 * Math.sin((2 * Math.PI * tb) / (1700 + 370 * b) + b);
  const band = [
    0.15 + 0.85 * kick, 0.15 + 0.75 * kick, 0.25 + 0.35 * kick,
    0.3 + 0.3 * wob(3), 0.3 + 0.35 * wob(4), 0.25 + 0.3 * wob(5),
    0.15 + 0.55 * hat, 0.1 + 0.6 * hat,
  ].map((v) => Math.round(Math.min(1, v) * 65536) / 65536);
  const level = Math.round(Math.min(1, 0.45 + 0.45 * kick + 0.1 * hat) * 65536) / 65536;
  return { band, level, beats: k + 1, beatms: Math.floor(t - k * period) };
}
