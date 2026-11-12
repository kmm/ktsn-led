import { describe, expect, it } from 'vitest';
import { findPalettes, topForms } from './inCode';
import { paletteCode } from './palette';

describe('palettes in code', () => {
  it('finds forms and skips comments', () => {
    expect(topForms('; hi\n(a (b c)) ; x\n(d)').map((f) => f.sx)).toEqual([['a', ['b', 'c']], ['d']]);
    expect(topForms('(a (b')).toEqual([]);
  });
  it('keeps strings whole, with what looks like code inside them', () => {
    expect(topForms('(a "x) ;(y" "q\\"r") (b)').map((f) => f.sx)).toEqual([['a', '"x) ;(y"', '"q\\"r"'], ['b']]);
    expect(topForms('(a "open').map((f) => f.sx)).toEqual([]);
    const pal = '(set (slice (buf G0) 0 2) 0 #FF0000 #0000FF)';
    expect(findPalettes(`(seg14 (buf X) "A) (B")\n${pal}`).map((p) => p.text)).toEqual([pal]);
  });
  it('recognises what the Palette tab writes, round trip', () => {
    const ten = Array.from({ length: 10 }, (_, i) => `#0000${(i * 16).toString(16).padStart(2, '0').toUpperCase()}`);
    for (const [stops, t] of [
      [['#FF0000', '#00FF00', '#0000FF40'], { buf: 0, start: 0, name: 'pal' }],
      [['#FF0000'], { buf: 3, start: 10, name: '' }],
      [ten, { buf: 1, start: 4, name: 'sky' }],
      [ten, { buf: 2, start: 0, name: '' }],
    ] as const) {
      const code = paletteCode([...stops], t);
      const src = `; before\n(fill (buf O) #000000)\n${code}(fill-grad (buf O) (slice (buf G0) 0 3))\n`;
      const found = findPalettes(src);
      expect(found).toHaveLength(1);
      expect(found[0]).toMatchObject({ stops: [...stops], target: t, text: code.trimEnd() });
      expect(src.slice(found[0].from, found[0].to)).toBe(code.trimEnd());
    }
  });
  it('takes lower case buffers and colours as written, and the example program', () => {
    const src = '(def pal (set (slice (buf g2) 0 3) 0 #ff0000 #00FF00 #0000ff))\n(fill-pat (buf O) pal (/ (env frame) 4) 3 t)';
    expect(findPalettes(src)[0]).toMatchObject({ stops: ['#ff0000', '#00FF00', '#0000ff'], target: { buf: 2, start: 0, name: 'pal' } });
  });
  it('leaves anything it could not write back alone', () => {
    expect(findPalettes('(set (slice (buf G0) 0 4) 0 #FF0000 #00FF00)')).toEqual([]);          // doesn't fill the slice
    expect(findPalettes('(set (slice (buf G0) 0 2) 1 #FF0000 #00FF00)')).toEqual([]);          // not from 0
    expect(findPalettes('(set (slice (buf O) 0 2) 0 #FF0000 #00FF00)')).toEqual([]);           // not a G buffer
    expect(findPalettes('(set (slice (buf G0) 0 2) 0 #FF0000 (get (buf G1) 0))')).toEqual([]); // not all colours
    expect(findPalettes('(def p (slice (buf G0) 0 3)) (set p 0 #FF0000 #00FF00)')).toEqual([]); // short
    expect(findPalettes('(def p (slice (buf G0) 0 3)) (set q 0 #FF0000 #00FF00 #0000FF)')).toEqual([]);
    expect(findPalettes('(set (slice (buf G0) 0 9) 0 #000001 #000002 #000003) (set (slice (buf G0) 0 9) 3 #000004)')).toEqual([]);
  });
  it('finds several, in order', () => {
    const src = paletteCode(['#FF0000', '#0000FF'], { buf: 0, start: 0, name: 'a' }) + '(fill (buf O) #000000)\n' + paletteCode(['#00FF00'], { buf: 1, start: 5, name: '' });
    expect(findPalettes(src).map((p) => p.target.buf)).toEqual([0, 1]);
  });
});
