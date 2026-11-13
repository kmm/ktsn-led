// Shapes of the board's JSON API (firmware main/api.c) and the transport interface both HTTP and serial implement.

export interface Stat { last: number; avg: number; max: number }

/** /api/status firmware: what is running; `ota` = POST /api/ota works (4 MB OTA layout). */
export interface FirmwareInfo {
  version: string; built: string; time: string; idf: string; target: string; driver: string; slot: string; ota: boolean;
  /** Running image: "pending" = a new update, not confirmed yet (a reset goes back), "valid", "" = flashed over USB. */
  state?: string;
  /** What is in the other app slot (OTA layouts), or null. */
  other?: { slot: string; version: string; built: string; time: string } | null;
}

export interface BoardStatus {
  uptime_s: number; fps: number; frames: number; missed: number;
  output_hz?: number; sends?: number;                // firmware with oversampled output
  intensity?: number;                                // global intensity now, 0..1 (mid-fade too)
  automation?: { active: boolean; every: number; runs: number; last_run?: string | null; last_run_uptime_s?: number;
                 faults: number; last_error: string | null };
  firmware?: FirmwareInfo;                           // firmware with version info (OTA builds and later)
  frame_us: Stat; flush_us: Stat;
  channels: { npix: number; nodes: number; faults: number; last_error: string | null; render_us: Stat; output_us: Stat; intensity?: number }[];
  time: string; tz: string; hostname: string;
  wifi: { ssid: string; connected: boolean; ip: string; rssi: number } | null;     // null on WiFi-less builds
  ntp?: { server: string; synced: boolean; last_sync: string | null };
  /** GPIO slots now (configured ones): value 0..1 (binary and digital inputs: 0 or 1; analog: the latest 16 ms
   *  averaged), ok = set up without errors; mode missing on firmware from before inputs. */
  io?: { n: number; name: string; mode?: BoardIo['mode']; value: number; ok: boolean }[];
  /** The audio input's analysis (null: none; missing on firmware from before 0.23.0): bands/level 0..1, beatms 2147483647 =
   *  no beat yet, swing in ADC counts, analysis time per 32 ms block. */
  audio?: { n: number; bands: number[]; level: number; beats: number; beatms: number; swing: number; adc_hz: number;
            blocks: number; us_avg: number; us_max: number; overflows: number } | null;
  heap_free: number;
}

/** A GPIO slot: an output (relay, PWM; always off at boot) or an input (digital, analog). Configuration applies at
 *  once, `save` keeps it. pull/debounce/trigger: digital inputs; missing on firmware from before inputs. */
export interface BoardIo {
  n: number; gpio: number; mode: 'binary' | 'pwm' | 'input' | 'analog' | 'audio'; freq: number; invert: boolean; name: string;
  pull?: 'up' | 'down' | 'none'; debounce?: number; trigger?: boolean;
}
export const isOutputMode = (m: BoardIo['mode']) => m === 'binary' || m === 'pwm';

/** Whether a firmware version ("MAJOR.MINOR.PATCH", maybe with a suffix) is at least `min`; false if unreadable. */
export function versionAtLeast(version: string | undefined, min: string): boolean {
  const parse = (v: string) => /^(\d+)\.(\d+)\.(\d+)/.exec(v)?.slice(1).map(Number);
  const a = version ? parse(version) : undefined, b = parse(min);
  if (!a || !b) return false;
  for (let i = 0; i < 3; i++) if (a[i] !== b[i]) return a[i] > b[i];
  return true;
}

/** The io modes a board offers: inputs came in 0.20.0, audio in 0.21.0. The firmware version decides; slots already
 *  using a mode (or reporting input fields) count too, for boards whose status has no version. */
export function boardIoModes(version: string | undefined, slots: { mode?: string; pull?: unknown }[]): BoardIo['mode'][] {
  const inputs = versionAtLeast(version, '0.20.0') || slots.some((s) => s.pull !== undefined || (s.mode !== undefined && !isOutputMode(s.mode as BoardIo['mode'])));
  const audio = versionAtLeast(version, '0.21.0') || slots.some((s) => s.mode === 'audio');
  return ['binary', 'pwm', ...(inputs ? ['input', 'analog'] as const : []), ...(audio ? ['audio'] as const : [])];
}

/** `wire`: bytes per pixel on the wire ("RGB", "GRB", "GRBW", "BRG WWW"; layout, applies after save + reboot);
 *  `gamma`: 1.0..3.0, 1 = off; `dither`: bits 0..4, 0 = off; `wmax`: white limit, percent (W spread over several W
 *  outputs when the wire has more than one). Missing on firmware from before them. */
export interface BoardChannel {
  gpio: number; npix: number; wire?: string; gamma?: number; dither?: number; wmax?: number;
  /** White from RGB: mode, white point ("#RRGGBB": what full W looks like), ratio (full W brightness vs that colour). */
  wmode?: 'direct' | 'extract' | 'boost'; wpoint?: string; wratio?: number;
  /** Channel intensity 0..1 (target), multiplied with the global one at output. */
  intensity?: number;
  running: boolean; prog: string | null;
}

export interface BoardOutput { driver: 'rmt' | 'parlio'; refresh: number; parallel: boolean }

export interface BoardConfig {
  channels: BoardChannel[];
  autoexec: string | null;
  /** The automation program (null = none) and its period in seconds; missing on firmware from before it. */
  automation?: string | null;
  automation_every?: number;
  intensity: number;
  /** Board-wide output stage; missing on firmware from before it. refresh: sends per rendered frame (1..4). */
  output?: BoardOutput;
  /** GPIO outputs (configured slots) and how many slots there are; missing on firmware from before them. */
  io?: BoardIo[];
  io_max?: number;
  tz: string; ntp: string; hostname: string;
  /** Where the board is, for env sunrise etc. (null = not set); missing on firmware from before it. */
  loc?: { lat: number; lon: number } | null;
  wifi: { ssid: string; password_set: boolean };
  token_set: boolean;
  reboot_needed: boolean;
}

/** PATCH /api/config body (all optional). See firmware api_patch_config. */
export interface ConfigPatch {
  intensity?: number;
  output?: { refresh?: number; parallel?: boolean };
  tz?: string; ntp?: string; hostname?: string;
  loc?: { lat: number; lon: number } | null;
  autoexec?: string | null;
  automation?: string | null;
  automation_every?: number;
  wifi?: { ssid: string; password?: string } | null;
  channels?: Record<string, { gpio?: number; npix?: number; wire?: string; gamma?: number; dither?: number; wmax?: number;
    wmode?: 'direct' | 'extract' | 'boost'; wpoint?: string; wratio?: number; intensity?: number; prog?: string | null } | null>;
  /** GPIO outputs by slot number: changed fields, or null to remove. */
  io?: Record<string, Partial<Omit<BoardIo, 'n'>> | null>;
  /** Set outputs now, by slot number or name: true / false, or a level 0..1 (PWM). */
  io_set?: Record<string, boolean | number>;
  /** Seconds over which this patch's intensity changes fade in. */
  fade?: number;
  save?: boolean;
}

export interface PatchResult { ok: true; saved: boolean; reboot_needed: boolean; note?: string; save_error?: string }

/** A non-2xx answer from the board. `fields` holds per-field errors of a rejected config patch. */
export class ApiError extends Error {
  constructor(readonly status: number, message: string, readonly fields?: Record<string, string>) { super(message); }
}

/** The board's program library (/api/lib): programs stored by name in its progs partition. */
export interface LibList { programs: { name: string; size: number }[]; used: number; total: number; separate: boolean }

export interface Transport {
  readonly kind: 'http' | 'serial';
  status(): Promise<BoardStatus>;
  config(): Promise<BoardConfig>;
  patch(body: ConfigPatch): Promise<PatchResult>;
  /** Compile and run on channel `ch` (running program only; `save` persists it). Throws ApiError 400 on compile errors. */
  prog(ch: number, src: string): Promise<void>;
  cmd(line: string): Promise<{ result: number; output: string }>;
  /** Program library: list, read, store (compiled on the board first: ApiError 400 on errors), delete. */
  libList(): Promise<LibList>;
  libGet(name: string): Promise<string>;
  libPut(name: string, src: string): Promise<void>;
  libDel(name: string): Promise<void>;
  /** Network firmware update (HTTP only): resolves with the new version; the board restarts afterwards. */
  ota?(image: Uint8Array, onProgress: (sent: number, total: number) => void): Promise<string>;
  save(): Promise<void>;
  reboot(): Promise<void>;
  close(): Promise<void>;
}

/** Turn an API error body into an ApiError. */
export function apiError(status: number, body: unknown): ApiError {
  const b = (body ?? {}) as { error?: string; errors?: Record<string, string> };
  if (b.errors) return new ApiError(status, Object.entries(b.errors).map(([k, v]) => `${k}: ${v}`).join('; '), b.errors);
  return new ApiError(status, b.error ?? `HTTP ${status}`);
}
