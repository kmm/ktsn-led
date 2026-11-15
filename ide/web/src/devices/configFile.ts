// Board files: export a board to JSON (everything, for cloning, or its programs only) and import it into the same or
// another board. Secrets are never in the file (the board never returns its WiFi password or token).
import type { BoardConfig, BoardIo, ConfigPatch } from './types';

export const FORMAT = 'ktsn-led-config';
/** 1: everything; 2: programs only (channels without gpio / npix). Full exports stay version 1 (their newer fields are
 *  optional), so older IDEs still read them. */
export const VERSION = 2;

/** A GPIO slot in a file: as the board's config reports it. */
export type FileIo = Pick<BoardIo, 'n' | 'gpio' | 'mode' | 'freq' | 'invert' | 'name' | 'pull' | 'debounce' | 'trigger'>;

export interface ConfigFile {
  format: typeof FORMAT;
  version: number;
  exported: string;                // ISO time
  device: string;                  // name in the IDE when exported
  /** Programs only: channel programs, autoexec, automation and library; no layout or settings. */
  programs_only?: boolean;
  config: {
    channels: { gpio?: number; npix?: number; wire?: string; gamma?: number; dither?: number; wmax?: number;
      wmode?: 'direct' | 'extract' | 'boost'; wpoint?: string; wratio?: number; intensity?: number; prog: string | null }[];   // wire..wratio: newer files
    autoexec: string | null;
    automation?: string | null;                         // newer files
    automation_every?: number;
    intensity: number;
    output?: { refresh: number; parallel: boolean };   // newer files
    tz: string; ntp: string; hostname: string;
    loc?: { lat: number; lon: number } | null;          // newer files
    wifi_ssid: string;
    library?: { name: string; src: string }[];          // newer files: the board's program library
    io?: FileIo[];                                      // newer files: GPIO slots
    audio_gate?: number;                                // newer files: the audio noise gate
  };
}

export function exportConfig(cfg: BoardConfig, device: string, now = new Date(), library?: { name: string; src: string }[]): ConfigFile {
  return {
    format: FORMAT, version: 1, exported: now.toISOString(), device,
    config: {
      channels: cfg.channels.map((c) => ({ gpio: c.gpio, npix: c.npix, ...(c.wire !== undefined ? { wire: c.wire } : {}),
        ...(c.gamma !== undefined ? { gamma: c.gamma } : {}),
        ...(c.dither !== undefined ? { dither: c.dither } : {}),
        ...(c.wmax !== undefined ? { wmax: c.wmax } : {}),
        ...(c.wmode !== undefined ? { wmode: c.wmode, wpoint: c.wpoint, wratio: c.wratio } : {}),
        ...(c.intensity !== undefined ? { intensity: c.intensity } : {}), prog: c.prog })),
      autoexec: cfg.autoexec, intensity: cfg.intensity,
      ...(cfg.automation !== undefined ? { automation: cfg.automation, automation_every: cfg.automation_every } : {}),
      ...(cfg.output ? { output: { refresh: cfg.output.refresh, parallel: cfg.output.parallel } } : {}),
      tz: cfg.tz, ntp: cfg.ntp, hostname: cfg.hostname,
      ...(cfg.loc !== undefined ? { loc: cfg.loc } : {}),
      wifi_ssid: cfg.wifi.ssid,
      ...(library ? { library } : {}),
      ...(cfg.io ? { io: cfg.io.map((x) => ({ n: x.n, gpio: x.gpio, mode: x.mode, freq: x.freq, invert: x.invert, name: x.name,
        ...(x.pull !== undefined ? { pull: x.pull, debounce: x.debounce, trigger: x.trigger } : {}) })) } : {}),
      ...(cfg.audio_gate !== undefined ? { audio_gate: cfg.audio_gate } : {}),
    },
  };
}

/** Programs only: each channel's program, the autoexec, the automation program and the library; no settings. */
export function exportPrograms(cfg: BoardConfig, device: string, now = new Date(), library?: { name: string; src: string }[]): ConfigFile {
  return {
    format: FORMAT, version: 2, exported: now.toISOString(), device, programs_only: true,
    config: {
      channels: cfg.channels.map((c) => ({ prog: c.prog })),
      autoexec: cfg.autoexec,
      ...(cfg.automation !== undefined ? { automation: cfg.automation, automation_every: cfg.automation_every } : {}),
      intensity: cfg.intensity, tz: '', ntp: '', hostname: '', wifi_ssid: '',
      ...(library ? { library } : {}),
    },
  };
}

export function fileName(cfg: BoardConfig, now = new Date(), programsOnly = false): string {
  const d = now.toISOString().slice(0, 16).replace(/[:T]/g, '-');
  return `ktsn-led-${cfg.hostname || 'board'}${programsOnly ? '-programs' : ''}-${d}.json`;
}

/** Parse and check an uploaded file; throws with a readable message. */
export function parseConfigFile(text: string): ConfigFile {
  let v: unknown;
  try { v = JSON.parse(text); } catch { throw new Error('not a JSON file'); }
  const f = v as Partial<ConfigFile>;
  if (f?.format !== FORMAT && f?.format !== 'ledctl-config')   // the name before 0.18.0
    throw new Error('not a KTSN LED config file');
  if (typeof f.version !== 'number' || f.version > VERSION) throw new Error(`config file version ${f.version} is newer than this IDE understands`);
  const c = f.config;
  if (!c || !Array.isArray(c.channels) || !c.channels.length) throw new Error('config file has no channels');
  for (const ch of c.channels) {
    if (f.programs_only ? typeof ch !== 'object' || ch === null : typeof ch?.gpio !== 'number' || typeof ch?.npix !== 'number')
      throw new Error('config file has a malformed channel');
  }
  if (c.io !== undefined && (!Array.isArray(c.io) || c.io.some((x) => typeof x?.n !== 'number' || typeof x?.gpio !== 'number' || typeof x?.mode !== 'string')))
    throw new Error('config file has a malformed GPIO slot');
  return f as ConfigFile;
}

export interface RestoreOptions {
  layout: boolean;       // channel GPIOs and pixel counts (apply after save + reboot)
  programs: boolean;
  library: boolean;      // library programs in the file are stored on the board (same names are replaced)
  autoexec: boolean;
  automation: boolean;   // the automation program and its period (after the library: play checks names)
  intensity: boolean;
  output: boolean;       // per-channel gamma and dither, board refresh and parallel sending (apply at once)
  time: boolean;         // tz + ntp + location
  hostname: boolean;     // off by default: two boards with one name collide on mDNS
  wifiSsid: boolean;     // off by default: the file has no password, the board keeps its current one
  io: boolean;           // GPIO slots (relays, PWM, inputs, audio): the file's slots, the board's others removed
  audioGate: boolean;    // the audio noise gate
  save: boolean;         // write to the board's flash too
}

export const DEFAULT_RESTORE: RestoreOptions = {
  layout: true, programs: true, library: true, autoexec: true, automation: true, intensity: true, output: true, time: true, hostname: false, wifiSsid: false, io: true, audioGate: true, save: true,
};

/** The options a file (and the board) can use at all: a programs-only file has no layout or settings; parts missing
 *  from older files or older firmware don't apply. */
export function restoreChoices(file: ConfigFile, board: BoardConfig): Set<keyof RestoreOptions> {
  const c = file.config;
  const k = new Set<keyof RestoreOptions>(['programs', 'autoexec', 'save']);
  if (c.library) k.add('library');
  if (c.automation !== undefined && board.automation !== undefined) k.add('automation');
  if (file.programs_only) return k;
  for (const x of ['layout', 'intensity', 'output', 'time', 'hostname', 'wifiSsid'] as const) k.add(x);
  if (c.io && board.io !== undefined) k.add('io');
  if (c.audio_gate !== undefined && board.audio_gate !== undefined) k.add('audioGate');
  return k;
}

/** After the reboot a new layout needs: what is left to do on the channels that now run (programs, their intensity and
 *  output settings), saved. */
export function afterReboot(o: RestoreOptions): RestoreOptions {
  const none = Object.fromEntries(Object.keys(o).map((k) => [k, false])) as unknown as RestoreOptions;
  return { ...none, programs: o.programs, intensity: o.intensity, output: o.output, save: true };
}

/** Build a restore. Programs only go to channels the board is running now (the board rejects programs for channels
 *  that don't exist yet, which would fail the whole patch); those are listed in `skipped`.
 *  Channels the file doesn't have are removed with separate patches, `removals` (highest first): the board removes
 *  only the last channel, and JSON objects can't carry {"2": null, "1": null} in that order (integer keys sort up). */
export function buildRestorePatch(file: ConfigFile, board: BoardConfig, opts: RestoreOptions, ioModes?: readonly string[]):
    { removals: ConfigPatch[]; patch: ConfigPatch; skipped: string[]; library: { name: string; src: string }[] } {
  const can = restoreChoices(file, board);
  const o = Object.fromEntries(Object.entries(opts).map(([k, v]) => [k, v && can.has(k as keyof RestoreOptions)])) as unknown as RestoreOptions;
  const patch: ConfigPatch = {};
  const removals: ConfigPatch[] = [];
  const skipped: string[] = [];
  const c = file.config;
  const channels: NonNullable<ConfigPatch['channels']> = {};
  const running = board.channels.filter((ch) => ch.running).length;

  if (o.layout) {
    // wire only if the board knows wire formats (older firmware rejects nothing but would ignore it anyway)
    c.channels.forEach((ch, i) => { channels[i] = { gpio: ch.gpio!, npix: ch.npix!, ...(ch.wire && board.channels[0]?.wire !== undefined ? { wire: ch.wire } : {}) }; });
    for (let i = board.channels.length - 1; i >= c.channels.length; i--) removals.push({ channels: { [i]: null } });
  }
  if (o.programs) {
    c.channels.forEach((ch, i) => {
      if (i < running) channels[i] = { ...(channels[i] ?? {}), prog: ch.prog ?? '' };
      else if (ch.prog) skipped.push(`program for channel ${i} (not running on the board yet: save + reboot, then restore again)`);
    });
  }
  if (o.output) {
    const exist = o.layout ? c.channels.length : board.channels.length;     // the board's configured channels after the patch
    c.channels.forEach((ch, i) => {
      if (i >= exist) return;
      if (typeof ch.gamma === 'number') channels[i] = { ...(channels[i] ?? {}), gamma: ch.gamma };
      if (typeof ch.dither === 'number') channels[i] = { ...(channels[i] ?? {}), dither: ch.dither };
      if (typeof ch.wmax === 'number' && board.channels[0]?.wmax !== undefined) channels[i] = { ...(channels[i] ?? {}), wmax: ch.wmax };
      if (ch.wmode && board.channels[0]?.wmode !== undefined)
        channels[i] = { ...(channels[i] ?? {}), wmode: ch.wmode, ...(ch.wpoint ? { wpoint: ch.wpoint } : {}), ...(ch.wratio ? { wratio: ch.wratio } : {}) };
    });
  }
  if (Object.keys(channels).length) patch.channels = channels;
  if (o.autoexec) patch.autoexec = c.autoexec;
  if (o.automation && c.automation !== undefined && board.automation !== undefined) {
    patch.automation = c.automation;
    if (c.automation_every) patch.automation_every = c.automation_every;
  }
  if (o.intensity) {
    patch.intensity = c.intensity;
    if (board.channels[0]?.intensity !== undefined)       // per channel: running channels only (the board refuses others)
      c.channels.forEach((ch, i) => {
        if (typeof ch.intensity === 'number' && i < running) channels[i] = { ...(channels[i] ?? {}), intensity: ch.intensity };
      });
    if (Object.keys(channels).length) patch.channels = channels;
  }
  if (o.output && c.output && board.output) patch.output = { refresh: c.output.refresh, parallel: c.output.parallel };
  if (o.time) { patch.tz = c.tz; patch.ntp = c.ntp; if (c.loc !== undefined && board.loc !== undefined) patch.loc = c.loc; }
  if (o.hostname) patch.hostname = c.hostname;
  if (o.wifiSsid && c.wifi_ssid) patch.wifi = { ssid: c.wifi_ssid };
  if (o.io && c.io) {          // the file's slots as they are (whole), the board's other slots removed
    const io: NonNullable<ConfigPatch['io']> = {};
    const max = board.io_max ?? 0;
    for (const x of c.io) {
      if (x.n >= max) { skipped.push(`GPIO slot ${x.n} (the board has ${max})`); continue; }
      if (ioModes && !ioModes.includes(x.mode)) { skipped.push(`GPIO slot ${x.n} (${x.mode}: not in this board's firmware)`); continue; }
      io[x.n] = { gpio: x.gpio, mode: x.mode, freq: x.freq, invert: x.invert, name: x.name,
        ...(x.pull !== undefined && (!ioModes || ioModes.includes('input')) ? { pull: x.pull, debounce: x.debounce, trigger: x.trigger } : {}) };
    }
    for (const y of board.io ?? []) if (!(y.n in io) && !c.io.some((x) => x.n === y.n)) io[y.n] = null;
    if (Object.keys(io).length) patch.io = io;
  }
  if (o.audioGate && c.audio_gate !== undefined) patch.audio_gate = c.audio_gate;
  if (o.save) patch.save = true;
  const library = o.library ? (c.library ?? []).filter((p) => /^[a-z][a-z0-9_-]{0,14}$/.test(p.name) && p.src.trim()) : [];
  return { removals, patch, skipped, library };
}
