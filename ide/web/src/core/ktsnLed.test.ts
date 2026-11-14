import { afterEach, beforeAll, describe, expect, it } from 'vitest';
import { averageSends, dayOfYear, envAt, pixelCssVia, pixelCssWhite, screenTable, skyClock } from './env';
import { type Core, loadCore, parseError } from './ktsnLed';

let core: Core;
const env = (frame = 1) => envAt(frame, new Date(2026, 9, 15, 12, 0, 7), 256);

beforeAll(async () => { core = await loadCore(); core.init(10); });

describe('compile errors', () => {
  it('accepts a valid program', () => { expect(core.check('(fill (buf O) #FF0000)')).toBeNull(); });
  it('reports line and column like a board', () => {
    expect(core.check('(fill (buf O) #FF0000')).toMatchObject({ line: 1, col: 1, message: "unclosed '('" });
    expect(core.check('\n\n  (nope)')).toMatchObject({ line: 3, col: 4 });
    expect(core.check('(fill (buf O) #FF0000 0 1.5)')?.message).toContain('use floor or round');
  });
  it('parses compiler messages', () => {
    expect(parseError('')).toBeNull();
    expect(parseError('line 2 col 7: bad color')).toEqual({ line: 2, col: 7, message: 'bad color', text: 'line 2 col 7: bad color' });
  });
});

describe('rendering', () => {
  it('renders the same pixels as the boards', () => {
    expect(core.load('(fill-grad (buf O) (set (slice (buf G0) 0 2) 0 #FF0000 #0000FF))')).toBeNull();
    core.render(env());
    const o = core.buffer('O');
    expect(o.length).toBe(10);
    expect(o[0]).toBe(0xff0000);
    expect(o[9]).toBe(0x0000ff);
    expect(o[1]).toBe(0xe3001c);                     // the gradient step the host tests also compute
    expect(core.buffer('G0')[1]).toBe(0x0000ff);
    expect(core.buffer('G0').length).toBe(256);      // global buffers are 256 px, like on the boards
  });
  it('picks from a list by the preview clock (list-pick)', () => {
    const src = '(fill (buf O) (list-pick (list #000000 #000001 #000002 #000003 #000004) 60))';
    expect(core.load(src)).toBeNull();
    const at = (m: number, sec: number) => { core.render(envAt(1, new Date(2026, 9, 15, 12, m, sec), 256)); return core.buffer('O')[0]; };
    const seq = Array.from({ length: 10 }, (_, m) => at(m, 5));
    expect(seq.every((v) => v <= 4)).toBe(true);
    expect(new Set(seq.slice(0, 5)).size + new Set(seq.slice(5)).size).toBeGreaterThanOrEqual(8);   // shuffled rounds
    for (let m = 1; m < 10; m++) expect(seq[m]).not.toBe(seq[m - 1]);                               // never twice in a row
    expect(at(3, 59)).toBe(seq[3]);                                                                  // same within a minute
  });
  it('sees the simulated env', () => {
    core.load('(fill (buf O) #000000) (fill (buf O) #00FF00 0 (/ (env second) 2))');   // second = 7 -> 3 px
    core.render(env());
    const o = core.buffer('O');
    expect([o[2], o[3]]).toEqual([0x00ff00, 0]);
  });
  it('keeps the old program when the new one does not compile', () => {
    core.load('(fill (buf O) #123456)');
    expect(core.load('(fill (buf O)')).not.toBeNull();
    core.render(env());
    expect(core.buffer('O')[0]).toBe(0x123456);
  });
  it('counts run-time faults instead of crashing', () => {
    core.load('(fill (buf O) #FF0000 0 (mod 5 (env second)))');
    core.render(envAt(1, new Date(2026, 0, 1, 0, 0, 0), 256));   // second = 0: division by zero
    expect(core.faults()).toBe(1);
    expect(core.lastError()).toBe('division by zero');
  });
  it('runs autoexec in a temporary channel that only leaves the globals behind', () => {
    expect(core.autoexec('(set (buf G3) 0 #ABCDEF)')).toBe('');
    expect(core.buffer('G3')[0]).toBe(0xabcdef);
    expect(core.autoexec('(fill (buf O)')).toContain("unclosed '('");
  });
  it('clears every buffer', () => {
    core.clear();
    expect(core.buffer('O').every((p) => p === 0) && core.buffer('G3').every((p) => p === 0)).toBe(true);
  });
});

describe('inputs', () => {
  afterEach(() => core.setOutputs(null));
  it('reads preview values by name offline, each name its own slot', () => {
    core.setOutputs(null);
    expect(core.load('(fill (buf O) #000000) (if (in button) (fill (buf O) #FF0000 0 2)) (fill (buf O) #0000FF 5 (+ 5 (round (* (analog knob) 5))))')).toBeNull();
    core.setInput('button', true);
    core.setInput('knob', 0.4);
    core.render(env());
    let o = core.buffer('O');
    expect([o[0], o[2], o[5], o[6], o[7]]).toEqual([0xff0000, 0, 0x0000ff, 0x0000ff, 0]);
    core.setInput('button', false);
    core.setInput('knob', 1);
    core.render(env(2));
    o = core.buffer('O');
    expect([o[0], o[9]]).toEqual([0, 0x0000ff]);
    expect(core.faults()).toBe(0);
  });
  it("checks a board's slots and their kinds", () => {
    core.setOutputs([{ n: 0, name: 'pump', mode: 'binary' }, { n: 3, name: 'button', mode: 'input' }, { n: 4, name: 'knob', mode: 'analog' }]);
    expect(core.check('(in button)')).toBeNull();
    expect(core.check('(analog knob 32)')).toBeNull();
    expect(core.check('(in knob)')?.text).toMatch(/analog input, not an input/);
    expect(core.check('(in pump)')?.text).toMatch(/an output, not an input/);
    expect(core.check('(in buttn)')?.text).toMatch(/no input by that name/);
    expect(core.check('(io-set button t)', 'automation')?.text).toMatch(/digital input: read it with/);
    expect(core.load('(if (in 3) (fill (buf O) #00FF00) (fill (buf O) #000000))')).toBeNull();
    core.setInput(3, true);
    core.render(env());
    expect(core.buffer('O')[0]).toBe(0x00ff00);
  });
});

describe('use', () => {
  const lib = '(defn half (x:decimal) (* x 0.5))';
  afterEach(() => core.setUseSources(new Map()));
  it('compiles against the sources it was given', () => {
    const p = '(use m)\n(def a (half 2.0))';
    expect(core.check(p)?.text).toMatch(/m/);
    core.setUseSources(new Map([['m', lib]]));
    expect(core.check(p)).toBeNull();
  });
  it('reports why a name can not be used', () => {
    core.setUseSources(new Map([['m', { why: 'two folders' }]]));
    expect(core.check('(use m)')?.text).toMatch(/two folders/);
  });
  it('shows an error in a library at the use, with the library line', () => {
    core.setUseSources(new Map([['m', '\n(defn half (x:decimal) (nosuch x))']]));
    const e = core.check('(use m)\n(def a (half 2.0))');
    expect(e?.line).toBe(1);
    expect(e?.text).toMatch(/in m line 2/);
  });
  it('checkUsing leaves the current sources alone', () => {
    core.setUseSources(new Map([['m', lib]]));
    expect(core.checkUsing('(use n)', new Map([['n', lib]]))).toBeNull();
    expect(core.useSources().has('m')).toBe(true);
    expect(core.check('(use n)')).not.toBeNull();
    expect(core.check('(use m)')).toBeNull();
  });
});

describe('sun and moon', () => {
  const day = Date.UTC(2026, 5, 21) / 1000;           // host/astro_cases.h: Greenwich sunrise 03:43 UTC
  it('computes like a board', () => {
    const t = core.sunEvent(51.48, 0, -0.8333, true, day, day + 86400)!;
    expect(Math.abs(t - day - 223 * 60)).toBeLessThanOrEqual(60);
    expect(core.sunEvent(69.65, 18.96, -0.8333, true, day, day + 86400)).toBeNull();     // midnight sun
    expect(core.moonLit(day + 43200)).toBeCloseTo(0.46, 1);
  });
  it('gives programs -1 without a location and values with one', () => {
    const sky = skyClock(core), when = new Date(2026, 5, 21, 12);
    expect(sky(when, null).sunrise).toBe(-1);
    expect(sky(when, null).moon).toBeGreaterThanOrEqual(0);
    const s = sky(when, { lat: 0, lon: -when.getTimezoneOffset() / 4 });                 // the equator, where the zone's clock is solar time
    expect(s.sunrise).toBeGreaterThan(5 * 60);
    expect(s.sunset).toBeLessThan(19 * 60);
    expect(s.dawn).toBeLessThan(s.sunrise);
    core.init(10);
    core.load('(if (gt (env sunset) 0) (fill (buf O) #FF0000))');
    core.render(envAt(1, when, 256, s));
    expect(core.buffer('O')[0] & 0xffffff).toBe(0xff0000);
    core.clear();
    core.render(envAt(2, when, 256));
    expect(core.buffer('O')[0] & 0xffffff).toBe(0);
  });
  it('follows the sun for daylight, each second', () => {
    const sky = skyClock(core), loc = { lat: 0, lon: -new Date(2026, 5, 21).getTimezoneOffset() / 4 };
    expect(sky(new Date(2026, 5, 21, 12), loc).daylight).toBe(1);
    expect(sky(new Date(2026, 5, 21, 0), loc).daylight).toBe(0);
    expect(sky(new Date(2026, 5, 21, 12), null).daylight).toBe(-1);
    const rise = sky(new Date(2026, 5, 21, 3), loc).sunrise;                 // within the minute either side of sunrise
    const at = (m: number) => sky(new Date(2026, 5, 21, 0, m), loc).daylight;
    expect([at(rise - 1), at(rise + 1)]).toEqual([0, 1]);
  });
  it('gives the time-of-day values a board computes', () => {
    core.init(10);
    const when = new Date(2026, 10, 11, 18, 0, 0, 250);                      // a Wednesday
    const read = (name: string) => {
      core.load(`(fill (buf O) #000000) (fill (buf O) #FF0000 0 (env ${name}))`);
      core.render(envAt(1, when, 256));
      return core.buffer('O').filter((p) => (p & 0xffffff) === 0xff0000).length;
    };
    expect(read('weekday')).toBe(3);
    expect(read('timeset')).toBe(1);
    core.load(`(fill (buf O) #000000) (if (and (eq (env dayfrac) 0.75) (eq (env daymin) 1080) (eq (env daysec) 64800)
      (eq (env ms) 250) (eq (floor (* (env minfrac) 1000)) 4)) (fill (buf O) #00FF00))`);   // 0.25 s of 60
    core.render(envAt(1, when, 256));
    expect(core.buffer('O')[0] & 0xffffff).toBe(0x00ff00);
    core.render(envAt(2, new Date(2026, 10, 11, 18, 0, 0, 0), 256));
    expect(core.buffer('O')[0] & 0xffffff).toBe(0);                            // ms 0 now
  });
});

describe('natives and env', () => {
  it('lists every function', () => {
    const names = core.natives().map((n) => n.name);
    expect(names).toContain('fill-pat');
    expect(names).toContain('rotate');
    expect(new Set(names).size).toBe(names.length);
  });
  it('computes day of year like a board', () => {
    expect(dayOfYear(new Date(2026, 0, 1))).toBe(1);
    expect(dayOfYear(new Date(2026, 11, 31))).toBe(365);
    expect(dayOfYear(new Date(2028, 11, 31))).toBe(366);
  });
});

describe('gamma and output', () => {
  it('builds the board\'s gamma tables', () => {
    const off = core.gammaLut(1);
    expect([...off]).toEqual([...Array(256).keys()]);
    const g = core.gammaLut(2.2);
    expect([g[0], g[1], g[64], g[128], g[255]]).toEqual([0, 0, 12, 56, 255]);      // same as main/gamma.c on the host
    expect([...core.gammaLut(5)]).toEqual([...off]);                              // out of range: off
  });
  it('maps buffer O like the board\'s output stage, then to the screen', () => {
    core.init(4);
    core.clear();
    core.load('(set (buf O) 0 #C88000 #000000 #010101 #FFFFFF)');
    core.render(envAt(1, new Date(), 256));
    const g22 = core.gammaLut(2.2);
    expect([...core.output(1, 256, 0, 0)]).toEqual([0xc88000, 0, 0x010101, 0xffffff]);          // nothing to do
    expect(core.output(1, 128, 0, 0)[0]).toBe(0x644000);                                       // px_scale rounding
    expect(core.output(1, 128.7, 0, 0)[0]).toBe(0x654000);  // fractions reach the board's 16-bit output intensity: 200 x 128.7/256 = 100.5 -> 101
    expect(core.output(2.2, 256, 0, 0)[0]).toBe((g22[0xc8] << 16) | (g22[0x80] << 8));
    // dither: black never lit, and the cycle average lands between the 8-bit steps
    let sum = 0;
    for (let f = 0; f < 16; f++) {
      const o = core.output(2.2, 256, 4, f);
      expect(o[1]).toBe(0);
      sum += (o[0] >>> 8) & 255;
    }
    expect(Math.abs(sum / 16 - 255 * Math.pow(0x80 / 255, 2.2))).toBeLessThan(1 / 16);
    // oversampled output: the average of 4 sends with 2 dither bits is the 8.8 value to within 1/8 step
    for (let base = 0; base < 64; base += 4) {
      const avg = averageSends([0, 1, 2, 3].map((j) => core.output(2.2, 256, 2, base + j)))[0];
      expect(Math.abs(((avg >>> 8) & 255) - 255 * Math.pow(0x80 / 255, 2.2))).toBeLessThan(0.51);
    }
    expect(averageSends([new Uint32Array([0x01020304])])[0]).toBe(0x01020304);
    expect(averageSends([new Uint32Array([0xff00ff00]), new Uint32Array([0x0100ff00])])[0]).toBe(0x8000ff00);
    // wire formats and white handling, as on a board (main/wire.h)
    core.load('(set (buf O) 0 #80808000 #FF0000 #000000FF #00000000)');
    core.render(envAt(2, new Date(), 256));
    const s14 = { wire: 'BRG WWW', mode: 'direct' as const, point: 0xffffff, ratio: 1, max: 100 };
    expect([...core.output(1, 256, 0, 0, s14)]).toEqual([0x808080, 0xff0000, 0xff000000, 0]);       // W level 255 = all W at full
    expect([...core.output(1, 256, 0, 0, { ...s14, mode: 'extract' })].slice(0, 2)).toEqual([0x80000000, 0xff0000]);  // grey -> W
    expect(core.output(1, 256, 0, 0, { ...s14, mode: 'boost' })[0]).toBe(0x80808080);
    expect(core.output(1, 256, 0, 0, { ...s14, max: 50 })[2] >>> 24).toBe(128);
    expect(core.output(1, 256, 0, 0, { ...s14, wire: 'RGB' })[2]).toBe(0);                          // no W outputs: no white
    expect(pixelCssWhite(0x80000000, 0xffffff, 1, false)).toBe('rgb(128,128,128)');
    expect(pixelCssWhite(0xff000000, 0xffb46b, 1, false)).toBe('rgb(255,180,107)');
    expect(pixelCssWhite(0xffff0000, 0xffffff, 2, false)).toBe('rgb(255,255,255)');                  // clamped
    expect(pixelCssWhite(0, 0xffffff, 1, true)).toBeNull();
    expect(screenTable(true)[56]).toBeGreaterThan(125);                                       // 2.2-corrected 128 ~ looks like 128
    expect(screenTable(true)[128]).toBeGreaterThan(180);                                      // uncorrected LEDs look washed out
    expect(screenTable(false)[77]).toBe(77);
    const t = screenTable(false);
    expect(pixelCssVia(0, t)).toBeNull();
    expect(pixelCssVia(0xff000000, t)).toBeNull();                                            // W only: dark on RGB boards
    expect(pixelCssVia(0x00ff8000, t)).toBe('rgb(255,128,0)');
    core.init(10);
  });
});
