import { beforeAll, describe, expect, it } from 'vitest';
import { NO_SKY } from '../core/env';
import { type Core, loadCore } from '../core/ktsnLed';
import { argCounts, CATEGORIES, ENTRIES, KEYWORDS, signature, TOPICS } from './entries';

let core: Core;
beforeAll(async () => {
  core = await loadCore();
  core.init(50);
  core.setUseSources(new Map([['shades', '(defn dim (c:color k:decimal) (lerp #000000 c k))']]));   // for use's example
});

describe('language reference', () => {
  it('documents every function the compiler knows, and nothing else (plus the keywords)', () => {
    const natives = core.natives().map((n) => n.name).sort();
    const documented = ENTRIES.map((e) => e.name).filter((n) => !KEYWORDS.includes(n)).sort();
    expect(documented).toEqual(natives);
    expect(new Set(ENTRIES.map((e) => e.name)).size).toBe(ENTRIES.length);
  });
  it('documents the same argument counts as the compiler', () => {
    for (const n of core.natives()) {
      const e = ENTRIES.find((x) => x.name === n.name)!;
      expect({ name: n.name, ...argCounts(e) }).toEqual({ name: n.name, min: n.minArgs, max: n.maxArgs });
    }
  });
  it('has examples that compile, and run without faults', () => {
    for (const e of [...ENTRIES, ...TOPICS.filter((t) => t.example)]) {
      const src = e.example!;
      const name = 'name' in e ? e.name : e.title;
      const ctx = 'context' in e && e.context === 'automation' ? 'automation' : 'render';
      expect({ name, err: core.check(src, ctx)?.text ?? null }).toEqual({ name, err: null });
      core.init(50);
      core.setContext(ctx);
      core.load(src);
      for (let f = 1; f <= 3; f++) core.render({ ...NO_SKY, frame: f, year: 2026, month: 10, day: 15, doy: 288, hour: 20, minute: 5, second: 7, ms: 0, weekday: 4, timeset: 1, intensity: 256 });
      expect({ name, faults: core.faults(), last: core.lastError() }).toEqual({ name, faults: 0, last: '' });
      core.setContext('render');
    }
  });
  it('puts every entry in a known category and gives it a summary', () => {
    for (const e of ENTRIES) {
      expect(CATEGORIES).toContain(e.category);
      expect(e.summary.length).toBeGreaterThan(5);
      expect(e.example.trim().length).toBeGreaterThan(0);
    }
  });
  it('writes signatures the way the docs do', () => {
    const fill = ENTRIES.find((e) => e.name === 'fill')!;
    expect(signature(fill)).toBe('(fill <buf> <color> [start] [end])');
    expect(signature(ENTRIES.find((e) => e.name === 'set')!)).toBe('(set <buf> <index> <color>…)');
  });
});
