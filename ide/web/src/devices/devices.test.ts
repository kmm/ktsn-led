import { describe, expect, it } from 'vitest';
import { afterReboot, buildRestorePatch, DEFAULT_RESTORE, exportConfig, exportPrograms, parseConfigFile, restoreChoices } from './configFile';
import { baseUrl, HttpTransport } from './http';
import { LineFramer, type Link, SerialTransport } from './serial';
import { ApiError, type BoardConfig } from './types';

/* ---------------- a simulated board on the other end of the serial line ---------------- */

const RS = '\x1e';

class FakeBoard implements Link {
  private onText: ((t: string) => void) | null = null;
  private inApi: boolean;
  private stop: (() => void) | null = null;
  writes: string[] = [];
  silent = false;
  lib = new Map<string, string>();

  constructor(startInApi = false, private rand = mulberry(42)) { this.inApi = startInApi; }

  async run(onText: (t: string) => void) {
    this.onText = onText;
    this.send('I (1234) boot: some log line\r\nled> ');
    await new Promise<void>((r) => { this.stop = r; });
  }
  async close() { this.stop?.(); }
  unplug() { this.stop?.(); }

  /** Send text in random small chunks, like USB packets, possibly splitting lines and frames. */
  private send(text: string) {
    let i = 0;
    while (i < text.length) {
      const n = 1 + Math.floor(this.rand() * 40);
      const chunk = text.slice(i, i + n);
      queueMicrotask(() => this.onText?.(chunk));
      i += n;
    }
  }

  async write(text: string) {
    this.writes.push(text);
    if (this.silent) return;
    for (const line of text.split('\n')) {
      if (!line) continue;
      if (!this.inApi) {
        this.send(`${line}\r\n`);                                 // console echo
        if (line === 'api') { this.inApi = true; this.send(`api mode: ...\r\n${RS}{"id":null,"status":200,"body":{"ready":true,"proto":1}}\n`); }
        else this.send('Unrecognized command\r\nled> ');
        continue;
      }
      if (line === 'exit') { this.inApi = false; this.send('left api mode\r\nled> '); continue; }
      const req = JSON.parse(line);
      const reply = (status: number, body: unknown) => this.send(`I (99) wifi: interleaved log\r\n${RS}${JSON.stringify({ id: req.id, status, body })}\n`);
      switch (req.op) {
        case 'status': reply(200, { fps: 60, uptime_s: 5, channels: [] }); break;
        case 'prog': {
          const depth = [...req.src].reduce((d: number, c: string) => d + (c === '(' ? 1 : c === ')' ? -1 : 0), 0);
          if (depth) reply(400, { error: "line 1 col 1: unclosed '('" }); else reply(200, { ok: true });
          break;
        }
        case 'cmd': reply(req.line === 'bogus' ? 400 : 200, { result: req.line === 'bogus' ? -1 : 0, output: req.line === 'bogus' ? "unknown command 'bogus'\n" : 'ch0: (fill)\n' }); break;
        case 'patch': reply(400, { errors: { intensity: 'must be an integer 0..256' } }); break;
        case 'lib': reply(200, { programs: [...this.lib].map(([name, src]) => ({ name, size: src.length })), used: 10, total: 100, separate: true }); break;
        case 'lib_get': this.lib.has(req.name) ? reply(200, { name: req.name, src: this.lib.get(req.name) }) : reply(404, { error: 'no such library program' }); break;
        case 'lib_put': if (req.src.includes('(fill')) { this.lib.set(req.name, req.src); reply(200, { ok: true }); } else reply(400, { error: 'line 1 col 1: bad' }); break;
        case 'lib_del': this.lib.delete(req.name); reply(200, { ok: true }); break;
        case 'lib_mv':
          if (!this.lib.has(req.name)) reply(404, { error: 'no such library program' });
          else if (this.lib.has(req.to)) reply(400, { error: 'a library program of that name exists (lib rm it first)' });
          else { this.lib.set(req.to, this.lib.get(req.name)!); this.lib.delete(req.name); reply(200, { ok: true, note: 'library program pals uses \'' + req.name + '\' and won\'t compile until changed' }); }
          break;
        case 'exit': this.inApi = false; reply(200, { pong: true }); break;
        default: reply(404, { error: 'unknown op' });
      }
    }
  }
}

function mulberry(seed: number) {
  return () => { seed |= 0; seed = (seed + 0x6d2b79f5) | 0; let t = Math.imul(seed ^ (seed >>> 15), 1 | seed);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t; return ((t ^ (t >>> 14)) >>> 0) / 4294967296; };
}

describe('serial line framing', () => {
  it('separates frames from log lines, across arbitrary chunk boundaries', () => {
    const frames: unknown[] = [], lines: string[] = [];
    const f = new LineFramer((x) => frames.push(x), (l) => lines.push(l));
    const text = `I (1) log\r\n\x1b[0;32mgreen\x1b[0m\r\n${RS}{"id":1,"status":200,"body":{"a":"x\\ny"}}\nled> api\r\n`;
    for (const ch of text) f.push(ch);                          // worst case: one character at a time
    expect(frames).toEqual([{ id: 1, status: 200, body: { a: 'x\ny' } }]);
    expect(lines).toEqual(['I (1) log', 'green', 'led> api']);
  });
  it('finds a frame that follows the prompt on the same line', () => {
    const frames: unknown[] = [];
    new LineFramer((x) => frames.push(x), () => {}).push(`led> api${RS}{"id":null,"status":200,"body":{"ready":true}}\n`);
    expect(frames).toHaveLength(1);
  });
});

describe('serial transport against a simulated board', () => {
  it('enters api mode, answers requests, keeps logs separate', async () => {
    const board = new FakeBoard();
    const logs: string[] = [];
    const t = await SerialTransport.connect(board, (l) => logs.push(l));
    expect((await t.status()).fps).toBe(60);
    expect(logs).toContain('I (1234) boot: some log line');
    expect(logs.some((l) => l.includes('interleaved log'))).toBe(true);
    await t.close();
  });
  it('recovers a board left in api mode by a previous session', async () => {
    const board = new FakeBoard(true);
    const t = await SerialTransport.connect(board, () => {});
    expect(board.writes[0]).toContain('exit');
    expect((await t.status()).fps).toBe(60);
    await t.close();
  });
  it('reports compile errors and field errors as ApiError', async () => {
    const t = await SerialTransport.connect(new FakeBoard(), () => {});
    await expect(t.prog(0, '(fill (buf O)')).rejects.toMatchObject({ status: 400, message: "line 1 col 1: unclosed '('" });
    await expect(t.prog(0, '(fill (buf O) #FF0000)')).resolves.toBeUndefined();
    const e = await t.patch({ intensity: 999 }).catch((x) => x);
    expect(e).toBeInstanceOf(ApiError);
    expect(e.fields).toEqual({ intensity: 'must be an integer 0..256' });
    await t.close();
  });
  it('stores, lists, reads and deletes library programs over serial, like HTTP', async () => {
    const board = new FakeBoard();
    const t = await SerialTransport.connect(board, () => {});
    await t.libPut('sunset', '(fill (buf O) #FF8000)');
    await expect(t.libPut('bad', '(nope')).rejects.toMatchObject({ status: 400 });
    expect((await t.libList()).programs).toEqual([{ name: 'sunset', size: 22 }]);
    expect(await t.libGet('sunset')).toBe('(fill (buf O) #FF8000)');
    await expect(t.libGet('gone')).rejects.toMatchObject({ status: 404 });
    expect(await t.libRename('sunset', 'dusk')).toMatch(/pals uses 'sunset'/);
    expect(await t.libGet('dusk')).toBe('(fill (buf O) #FF8000)');
    await t.libPut('other', '(fill (buf O) #000000)');
    await expect(t.libRename('dusk', 'other')).rejects.toMatchObject({ status: 400 });
    await expect(t.libRename('gone', 'x')).rejects.toMatchObject({ status: 404 });
    expect(await t.libDel('dusk')).toBeUndefined();
    await t.libDel('other');
    expect((await t.libList()).programs).toEqual([]);
    await t.close();
  });
  it('returns the output of failing commands instead of throwing', async () => {
    const t = await SerialTransport.connect(new FakeBoard(), () => {});
    expect(await t.cmd('list 0')).toEqual({ result: 0, output: 'ch0: (fill)\n' });
    expect(await t.cmd('bogus')).toEqual({ result: -1, output: "unknown command 'bogus'\n" });
    await t.close();
  });
  it('runs requests one at a time, in order', async () => {
    const board = new FakeBoard();
    const t = await SerialTransport.connect(board, () => {});
    const n = board.writes.length;
    await Promise.all([t.status(), t.cmd('list 0'), t.status()]);
    expect(board.writes.slice(n).map((w) => JSON.parse(w).op)).toEqual(['status', 'cmd', 'status']);
    await t.close();
  });
  it('fails cleanly when the board does not answer, or goes away', async () => {
    const board = new FakeBoard();
    const t = await SerialTransport.connect(board, () => {}, 200);
    board.silent = true;
    await expect(t.status()).rejects.toThrow(/no answer to "status"/);
    board.silent = false;
    const pending = t.status();
    board.unplug();
    await expect(pending.then(() => t.status())).rejects.toThrow(/closed/);
    await t.close();
  });
  it('gives up the handshake on something that is not a KTSN LED board', async () => {
    const silent = new FakeBoard();
    silent.silent = true;
    await expect(SerialTransport.connect(silent, () => {})).rejects.toThrow(/did not answer/);
  }, 8000);
});

/* ---------------- HTTP ---------------- */

function mockFetch(handler: (url: string, init: RequestInit) => Response) {
  const calls: { url: string; init: RequestInit }[] = [];
  const f = (async (url: string, init: RequestInit) => { calls.push({ url, init }); return handler(url, init); }) as unknown as typeof fetch;
  return { f, calls };
}

describe('http transport', () => {
  it('normalizes hosts', () => {
    expect(baseUrl('ktsn-led.local')).toBe('http://ktsn-led.local');
    expect(baseUrl('10.0.0.5:8080/')).toBe('http://10.0.0.5:8080');
    expect(baseUrl('https://x.y')).toBe('https://x.y');
  });
  it('sends the token and parses JSON', async () => {
    const { f, calls } = mockFetch(() => Response.json({ fps: 59 }));
    const t = new HttpTransport('ktsn-led.local', 'secret12', f);
    expect((await t.status()).fps).toBe(59);
    expect(calls[0].url).toBe('http://ktsn-led.local/api/status');
    expect((calls[0].init.headers as Record<string, string>).Authorization).toBe('Bearer secret12');
  });
  it('puts programs as text and surfaces compile errors', async () => {
    const { f, calls } = mockFetch(() => Response.json({ error: "line 1 col 1: unclosed '('" }, { status: 400 }));
    const t = new HttpTransport('b', 'secret12', f);
    await expect(t.prog(1, '(fill')).rejects.toMatchObject({ status: 400, message: "line 1 col 1: unclosed '('" });
    expect(calls[0].url).toBe('http://b/api/prog/1');
    expect(calls[0].init.body).toBe('(fill');
  });
  it('reads command output and its result header, also for failing commands', async () => {
    const { f } = mockFetch(() => new Response('unknown command\n', { status: 400, headers: { 'X-Ledctl-Result': '-1' } }));
    expect(await new HttpTransport('b', 'x', f).cmd('bogus')).toEqual({ result: -1, output: 'unknown command\n' });
  });
  it('reads, stores and deletes library programs', async () => {
    const { f, calls } = mockFetch((url, init) => (init.method === 'GET' && url.endsWith('/api/lib') ? Response.json({ programs: [{ name: 'sunset', size: 12 }], used: 9, total: 100, separate: true })
      : init.method === 'GET' ? Response.json({ name: 'sunset', src: '(fill (buf O) #FF8000)' }) : Response.json({ ok: true })));
    const t = new HttpTransport('b', 'x', f);
    expect((await t.libList()).programs).toEqual([{ name: 'sunset', size: 12 }]);
    expect(await t.libGet('sunset')).toBe('(fill (buf O) #FF8000)');
    await t.libPut('sunset', '(fill (buf O) #FF0000)');
    await t.libDel('sunset');
    await t.libRename('sunset', 'dusk');
    expect(calls.map((c) => `${c.init.method} ${c.url}`)).toEqual(['GET http://b/api/lib', 'GET http://b/api/lib/sunset', 'PUT http://b/api/lib/sunset',
      'DELETE http://b/api/lib/sunset', 'PATCH http://b/api/lib/sunset']);
    expect(calls[2].init.body).toBe('(fill (buf O) #FF0000)');
    expect(JSON.parse(calls[4].init.body as string)).toEqual({ name: 'dusk' });
  });
  it('explains auth and network failures', async () => {
    const { f } = mockFetch(() => Response.json({ error: 'missing or wrong token' }, { status: 401 }));
    await expect(new HttpTransport('b', 'x', f).status()).rejects.toMatchObject({ status: 401 });
    const down = (async () => { throw new TypeError('Failed to fetch'); }) as unknown as typeof fetch;
    await expect(new HttpTransport('b', 'x', down).status()).rejects.toThrow(/can't reach http:\/\/b/);
  });
});

/* ---------------- config files ---------------- */

const board: BoardConfig = {
  channels: [
    { gpio: 22, npix: 50, running: true, prog: '(fill (buf O) #FF0000)' },
    { gpio: 23, npix: 50, running: true, prog: null },
    { gpio: 21, npix: 10, running: true, prog: null },
  ],
  autoexec: '(set (buf G0) 0 #FFFFFF)', intensity: 200, tz: 'MST7MDT,M3.2.0,M11.1.0', ntp: 'pool.ntp.org', hostname: 'strip-a',
  wifi: { ssid: 'home', password_set: true }, token_set: true, reboot_needed: false,
};

describe('config files', () => {
  it('round-trips through export and parse, without secrets', () => {
    const text = JSON.stringify(exportConfig(board, 'Strip A', new Date('2026-10-15T12:00:00Z')));
    expect(text).not.toMatch(/password|token/);
    const f = parseConfigFile(text);
    expect(f.config.channels).toHaveLength(3);
    expect(f.config.hostname).toBe('strip-a');
  });
  it('rejects files that are not KTSN LED configs, reads ones from before the rename', () => {
    expect(() => parseConfigFile('nope')).toThrow(/not a JSON file/);
    expect(() => parseConfigFile('{"format":"other"}')).toThrow(/not a KTSN LED config/);
    expect(parseConfigFile(JSON.stringify({ ...JSON.parse(JSON.stringify(exportConfig(board, 'old'))), format: 'ledctl-config' })).config.channels).toHaveLength(3);
    expect(() => parseConfigFile('{"format":"ktsn-led-config","version":99,"config":{"channels":[{"gpio":1,"npix":1}]}}')).toThrow(/newer/);
  });
  it('restores to a board with more channels: removes the extra ones last-first, in separate patches', () => {
    const file = exportConfig({ ...board, channels: board.channels.slice(0, 1) }, 'one');
    const bigger = { ...board, channels: [...board.channels, { gpio: 19, npix: 5, running: true, prog: null }] };
    const { removals, patch } = buildRestorePatch(file, bigger, DEFAULT_RESTORE);
    expect(removals).toEqual([{ channels: { 3: null } }, { channels: { 2: null } }, { channels: { 1: null } }]);
    expect(patch.channels).toEqual({ 0: { gpio: 22, npix: 50, prog: '(fill (buf O) #FF0000)' } });
    expect(patch.save).toBe(true);
  });
  it('skips programs for channels the board is not running yet', () => {
    const file = exportConfig(board, 'three');
    const small = { ...board, channels: [{ gpio: 22, npix: 50, running: true, prog: null }] };
    const withProg = { ...file, config: { ...file.config, channels: file.config.channels.map((c, i) => ({ ...c, prog: i === 2 ? '(x)' : c.prog })) } };
    const { patch, skipped } = buildRestorePatch(withProg, small, DEFAULT_RESTORE);
    expect(patch.channels?.[0]).toMatchObject({ prog: '(fill (buf O) #FF0000)' });
    expect(patch.channels?.[2]).toEqual({ gpio: 21, npix: 10 });        // layout yes, program no
    expect(skipped).toEqual([expect.stringContaining('channel 2')]);
  });
  it('restores gamma and dither to channels that exist, and files from before gamma restore without it', () => {
    const g = { ...board, channels: board.channels.map((c, i) => ({ ...c, gamma: i === 0 ? 2.2 : 1, dither: i === 0 ? 3 : 0 })) };
    const file = exportConfig(g, 'g');
    expect(file.config.channels[0]).toMatchObject({ gamma: 2.2, dither: 3 });
    const small = { ...board, channels: [{ gpio: 22, npix: 50, running: true, prog: null }] };
    const onlyGamma = { ...DEFAULT_RESTORE, layout: false, programs: false };
    expect(buildRestorePatch(file, small, onlyGamma).patch.channels).toEqual({ 0: { gamma: 2.2, dither: 3 } });   // ch1, ch2 don't exist
    expect(buildRestorePatch(file, small, DEFAULT_RESTORE).patch.channels?.[2]).toEqual({ gpio: 21, npix: 10, gamma: 1, dither: 0 });
    expect(buildRestorePatch(exportConfig(board, 'old'), board, onlyGamma).patch.channels).toBeUndefined();
  });
  it('restores wire formats with the layout, only to boards that know them', () => {
    const wired = { ...board, channels: board.channels.map((c, i) => ({ ...c, wire: i === 0 ? 'BRG WWW' : 'RGB' })) };
    const file = exportConfig(wired, 'w');
    expect(file.config.channels[0].wire).toBe('BRG WWW');
    expect(buildRestorePatch(file, wired, DEFAULT_RESTORE).patch.channels?.[0]).toMatchObject({ gpio: 22, npix: 50, wire: 'BRG WWW' });
    expect(buildRestorePatch(file, board, DEFAULT_RESTORE).patch.channels?.[0]).not.toHaveProperty('wire');     // older firmware
    expect(buildRestorePatch(file, wired, { ...DEFAULT_RESTORE, layout: false }).patch.channels?.[0]).not.toHaveProperty('wire');
    const white = { ...wired, channels: wired.channels.map((c) => ({ ...c, wmax: 80, wmode: 'extract' as const, wpoint: '#FFA957', wratio: 1.5 })) };
    expect(buildRestorePatch(exportConfig(white, 'x'), white, DEFAULT_RESTORE).patch.channels?.[0])
      .toMatchObject({ wmax: 80, wmode: 'extract', wpoint: '#FFA957', wratio: 1.5 });
    expect(buildRestorePatch(exportConfig(white, 'x'), wired, DEFAULT_RESTORE).patch.channels?.[0]).not.toHaveProperty('wmode');
  });
  it('restores channel intensities to running channels', () => {
    const b = { ...board, channels: board.channels.map((c, i) => ({ ...c, intensity: [0.5, 1, 0.25][i] })) };
    const file = exportConfig(b, 'i');
    expect(file.config.channels.map((c) => c.intensity)).toEqual([0.5, 1, 0.25]);
    const small = { ...b, channels: [{ ...b.channels[0], running: true }, { ...b.channels[1], running: false }] };
    const p = buildRestorePatch(file, small, { ...DEFAULT_RESTORE, layout: false, programs: false, output: false }).patch;
    expect(p.channels).toEqual({ 0: { intensity: 0.5 } });
    expect(buildRestorePatch(file, board, { ...DEFAULT_RESTORE, layout: false, programs: false, output: false }).patch.channels).toBeUndefined();
  });
  it('restores the automation program and its period, only to boards that have automation', () => {
    const b = { ...board, automation: '(play 0 sunset)', automation_every: 30 };
    const file = exportConfig(b, 'a');
    expect(file.config).toMatchObject({ automation: '(play 0 sunset)', automation_every: 30 });
    expect(buildRestorePatch(file, b, DEFAULT_RESTORE).patch).toMatchObject({ automation: '(play 0 sunset)', automation_every: 30 });
    expect(buildRestorePatch(file, board, DEFAULT_RESTORE).patch.automation).toBeUndefined();          // older firmware
    expect(buildRestorePatch(file, b, { ...DEFAULT_RESTORE, automation: false }).patch.automation).toBeUndefined();
  });
  it('saves and restores the program library', () => {
    const lib = [{ name: 'sunset', src: '(fill (buf O) #FF8000)' }, { name: 'Bad Name', src: '(x)' }, { name: 'empty', src: '  ' }];
    const file = exportConfig(board, 'l', new Date(), lib);
    expect(file.config.library).toHaveLength(3);
    expect(buildRestorePatch(file, board, DEFAULT_RESTORE).library).toEqual([lib[0]]);           // invalid/empty skipped
    expect(buildRestorePatch(file, board, { ...DEFAULT_RESTORE, library: false }).library).toEqual([]);
    expect(exportConfig(board, 'old').config.library).toBeUndefined();
  });
  it('restores the board output stage when the file and the board both have one', () => {
    const withOut = { ...board, output: { driver: 'rmt' as const, refresh: 3, parallel: true } };
    const file = exportConfig(withOut, 'o');
    expect(file.config.output).toEqual({ refresh: 3, parallel: true });
    expect(buildRestorePatch(file, withOut, DEFAULT_RESTORE).patch.output).toEqual({ refresh: 3, parallel: true });
    expect(buildRestorePatch(file, board, DEFAULT_RESTORE).patch.output).toBeUndefined();             // older firmware
    expect(buildRestorePatch(file, withOut, { ...DEFAULT_RESTORE, output: false }).patch.output).toBeUndefined();
    expect(exportConfig(board, 'old').config.output).toBeUndefined();
  });
  it('leaves hostname and wifi alone unless asked, and can skip everything else', () => {
    const file = exportConfig(board, 'x');
    const { patch } = buildRestorePatch(file, board, DEFAULT_RESTORE);
    expect(patch.hostname).toBeUndefined();
    expect(patch.wifi).toBeUndefined();
    const none = buildRestorePatch(file, board, { layout: false, programs: false, library: false, autoexec: false, automation: false, intensity: false, output: false, time: false,
      hostname: true, wifiSsid: true, io: false, audioGate: false, save: false });
    expect(none.patch).toEqual({ hostname: 'strip-a', wifi: { ssid: 'home' } });
  });
  it('exports and restores GPIO slots whole, removing other slots on the board, within its slots and modes', () => {
    const io = [{ n: 0, gpio: 4, mode: 'binary' as const, freq: 1000, invert: false, name: 'pump', pull: 'up' as const, debounce: 20, trigger: false },
      { n: 2, gpio: 34, mode: 'audio' as const, freq: 1000, invert: false, name: 'mic', pull: 'none' as const, debounce: 20, trigger: false }];
    const b = { ...board, io, io_max: 4, audio_gate: 18 };
    const file = exportConfig(b, 'io');
    expect(file.config.io).toEqual(io);
    expect(file.config.audio_gate).toBe(18);
    expect(file.version).toBe(1);                // full exports stay readable by older IDEs
    const other = { ...board, io: [{ n: 1, gpio: 5, mode: 'pwm' as const, freq: 500, invert: true, name: 'fan' }], io_max: 4, audio_gate: 0 };
    const { patch, skipped } = buildRestorePatch(file, other, DEFAULT_RESTORE);
    expect(patch.io).toEqual({ 0: { gpio: 4, mode: 'binary', freq: 1000, invert: false, name: 'pump', pull: 'up', debounce: 20, trigger: false },
      1: null, 2: { gpio: 34, mode: 'audio', freq: 1000, invert: false, name: 'mic', pull: 'none', debounce: 20, trigger: false } });
    expect(patch.audio_gate).toBe(18);
    expect(skipped).toEqual([]);
    const old = buildRestorePatch(file, { ...other, io_max: 2 }, DEFAULT_RESTORE, ['binary', 'pwm']);   // fewer slots, no inputs
    expect(old.patch.io).toEqual({ 0: { gpio: 4, mode: 'binary', freq: 1000, invert: false, name: 'pump' }, 1: null });
    expect(old.skipped).toEqual([expect.stringContaining('GPIO slot 2')]);
    expect(buildRestorePatch(file, other, { ...DEFAULT_RESTORE, io: false, audioGate: false }).patch).not.toHaveProperty('io');
    expect(buildRestorePatch(file, board, DEFAULT_RESTORE).patch).not.toHaveProperty('audio_gate');   // older firmware
    expect(parseConfigFile(JSON.stringify(file)).config.io).toHaveLength(2);
    expect(() => parseConfigFile(JSON.stringify({ ...file, config: { ...file.config, io: [{ n: 0 }] } }))).toThrow(/GPIO slot/);
  });
  it('exports programs only, and restores just those (no layout or settings) from such a file', () => {
    const b = { ...board, channels: board.channels.map((c, i) => ({ ...c, running: i < 2, prog: i === 2 ? '(x)' : c.prog })),
      automation: '(play 0 sunset)', automation_every: 30, io: [], io_max: 4, audio_gate: 5 };
    const lib = [{ name: 'sunset', src: '(fill (buf O) #FF8000)' }];
    const file = parseConfigFile(JSON.stringify(exportPrograms(b, 'p', new Date(), lib)));
    expect(file.programs_only).toBe(true);
    expect(file.version).toBe(2);
    expect(file.config.channels).toEqual(b.channels.map((c) => ({ prog: c.prog })));
    expect(JSON.stringify(file)).not.toMatch(/strip-a|home|gpio/);
    expect([...restoreChoices(file, b)].sort()).toEqual(['autoexec', 'automation', 'library', 'programs', 'save']);
    const r = buildRestorePatch(file, b, { ...DEFAULT_RESTORE, hostname: true, wifiSsid: true });
    expect(r.removals).toEqual([]);
    expect(r.patch).toEqual({ channels: { 0: { prog: '(fill (buf O) #FF0000)' }, 1: { prog: '' } }, autoexec: b.autoexec,
      automation: '(play 0 sunset)', automation_every: 30, save: true });
    expect(r.skipped).toEqual([expect.stringContaining('channel 2')]);
    expect(r.library).toEqual(lib);
  });
  it('after the reboot a new layout needs: programs, intensities and output for the channels now running, saved', () => {
    const o = afterReboot({ ...DEFAULT_RESTORE, output: false });
    expect(o).toEqual({ ...Object.fromEntries(Object.keys(DEFAULT_RESTORE).map((k) => [k, false])), programs: true, intensity: true, save: true });
    const file = exportConfig({ ...board, channels: board.channels.map((c, i) => ({ ...c, prog: `(p${i})` })) }, 'x');
    const p = buildRestorePatch(file, { ...board, channels: board.channels.map((c) => ({ ...c, running: true })) }, o);
    expect(p.patch).toEqual({ channels: { 0: { prog: '(p0)' }, 1: { prog: '(p1)' }, 2: { prog: '(p2)' } }, intensity: board.intensity, save: true });
    expect(p.skipped).toEqual([]);
  });
});
