import { describe, expect, it } from 'vitest';
import { NO_SKY } from '../core/env';
import { loadCore } from '../core/ktsnLed';
import { parseHex } from '../color/colors';
import { gradient, loadPalettes, paletteCode, pattern } from './palette';

const c = (s: string) => parseHex(s)!;

describe('palettes', () => {
  it('blends like fill-grad on the board (ends exact, board rounding)', () => {
    const g = gradient([c('#FF0000'), c('#00FF00'), c('#0000FF')], 5);           // R, R/G, G, G/B, B
    expect(g).toEqual([c('#FF0000'), c('#808000'), c('#00FF00'), c('#008080'), c('#0000FF')]);
    expect(gradient([c('#00000080')], 3)).toEqual([c('#00000080'), c('#00000080'), c('#00000080')]);
    expect(gradient([c('#000000'), c('#000000FF')], 3)[1]).toEqual(c('#00000080'));  // W blends too
    expect(gradient([c('#FF0000'), c('#0000FF')], 1)).toEqual([c('#FF0000')]);
    expect(gradient([], 4)).toEqual([]);
  });
  it('repeats like fill-pat', () => {
    expect(pattern([c('#FF0000'), c('#0000FF')], 5).map((p) => p.r)).toEqual([255, 0, 255, 0, 255]);
    expect(pattern([c('#FF0000'), c('#0000FF')], 5, 2).map((p) => p.b)).toEqual([0, 0, 255, 255, 0]);
  });
  it('writes the code: one set per 8 colours, def when named', () => {
    expect(paletteCode(['#FF0000', '#00ff00', '#0000FF40'], { buf: 0, start: 0, name: 'pal' }))
      .toBe('(def pal (set (slice (buf G0) 0 3) 0 #FF0000 #00FF00 #0000FF40))\n');
    expect(paletteCode(['#FF0000'], { buf: 3, start: 10, name: '' })).toBe('(set (slice (buf G3) 10 11) 0 #FF0000)\n');
    const ten = Array.from({ length: 10 }, (_, i) => `#0000${(i * 16).toString(16).padStart(2, '0')}`);
    expect(paletteCode(ten, { buf: 1, start: 4, name: 'sky' })).toBe(
      '(def sky (slice (buf G1) 4 14))\n(set sky 0 #000000 #000010 #000020 #000030 #000040 #000050 #000060 #000070)\n(set sky 8 #000080 #000090)\n');
    expect(paletteCode(ten, { buf: 1, start: 0, name: '' }).split('\n')[1]).toBe('(set (slice (buf G1) 0 10) 8 #000080 #000090)');
    expect(paletteCode(['#FF0000'], { buf: 0, start: 0, name: 'nil' })).toBe('(set (slice (buf G0) 0 1) 0 #FF0000)\n');   // not a usable name
    expect(paletteCode([], { buf: 0, start: 0, name: 'p' })).toBe('');
  });
  it('loads saved palettes, ignoring broken entries', () => {
    const s = { getItem: () => JSON.stringify([{ name: 'fire', stops: ['#FF0000', '#FFFF00'] }, { name: 'bad', stops: ['nope'] }, 7]) };
    expect(loadPalettes(s)).toEqual([{ name: 'fire', stops: ['#FF0000', '#FFFF00'] }]);
  });
});

describe('palette code on the real compiler', () => {
  it('compiles and fills the G buffer as shown', async () => {
    const core = await loadCore();
    core.init(20);
    const stops = Array.from({ length: 11 }, (_, i) => `#${(i * 20).toString(16).padStart(2, '0')}0000${i === 3 ? '40' : ''}`);
    for (const name of ['pal', '']) {
      const src = paletteCode(stops, { buf: 2, start: 5, name }) + (name ? `(fill-grad (buf O) ${name})` : '');
      expect(core.check(src)).toBeNull();
    }
    core.clear();
    core.load(paletteCode(stops, { buf: 2, start: 5, name: 'pal' }));
    core.render({ ...NO_SKY, frame: 1, year: 2026, month: 10, day: 22, doy: 295, hour: 12, minute: 0, second: 0, ms: 0, weekday: 4, timeset: 1, intensity: 256 });
    const g2 = core.buffer('G2');
    expect([...g2.slice(5, 16)].map((p) => p >>> 0)).toEqual(stops.map((s) => { const c = parseHex(s)!; return ((c.w << 24) | (c.r << 16) | (c.g << 8) | c.b) >>> 0; }));
    expect(g2[4]).toBe(0);
  });
});
