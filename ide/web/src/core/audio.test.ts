import { describe, expect, it } from 'vitest';
import { fakeAudio, usesAudio } from './audio';

describe('usesAudio', () => {
  it('finds audio env names, not other env names, comments or strings', () => {
    expect(usesAudio('(fill (buf O) #FF0000 0 (round (* (env band0) 10)))')).toBe(true);
    expect(usesAudio('(if (lt (env beatms) 80) (fill (buf O) #FFFFFF))')).toBe(true);
    expect(usesAudio('(def l (env level))')).toBe(true);
    expect(usesAudio('(env  beats)')).toBe(true);
    expect(usesAudio('(env frame) (env band8) (env levels)')).toBe(false);
    expect(usesAudio('; (env band0)\n(str-cat "(env level)" "x")')).toBe(false);
  });
});

describe('fakeAudio', () => {
  it('beats on the tempo, beatms since the latest', () => {
    expect(fakeAudio(0, 120)).toMatchObject({ beats: 1, beatms: 0 });
    expect(fakeAudio(499, 120)).toMatchObject({ beats: 1, beatms: 499 });
    expect(fakeAudio(500, 120)).toMatchObject({ beats: 2, beatms: 0 });
    expect(fakeAudio(10_250, 120)).toMatchObject({ beats: 21, beatms: 250 });
    expect(fakeAudio(1000, 60)).toMatchObject({ beats: 2, beatms: 0 });
    expect(fakeAudio(1000, 1000).beats).toBe(5);                     // clamped to 240 bpm
  });
  it('bass jumps on the beat and falls, values in 0..1, steady within a block', () => {
    const on = fakeAudio(5000, 120), later = fakeAudio(5300, 120);
    expect(on.band[0]).toBeGreaterThan(0.8);
    expect(later.band[0]).toBeLessThan(0.3);
    expect(on.level).toBeGreaterThan(later.level);
    for (let t = 0; t < 4000; t += 7) {
      const a = fakeAudio(t, 128);
      expect(a.band).toHaveLength(8);
      for (const v of [...a.band, a.level]) { expect(v).toBeGreaterThanOrEqual(0); expect(v).toBeLessThanOrEqual(1); }
    }
    expect(fakeAudio(3201, 120).band).toEqual(fakeAudio(3231, 120).band);   // one 32 ms block
    expect(fakeAudio(1234, 97)).toEqual(fakeAudio(1234, 97));
  });
});
