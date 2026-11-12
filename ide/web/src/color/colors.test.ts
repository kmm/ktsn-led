import { describe, expect, it } from 'vitest';
import { addFavorite, FAVORITE_SLOTS, formatHex, loadFavorites, parseHex } from './colors';

const mem = (o: Record<string, string> = {}) => ({ getItem: (k: string) => o[k] ?? null, setItem: (k: string, v: string) => { o[k] = v; } });

describe('colours', () => {
  it('parses and formats #RRGGBB and #RRGGBBWW', () => {
    expect(parseHex('#FF8000')).toEqual({ r: 255, g: 128, b: 0, w: 0 });
    expect(parseHex('#ff800040')).toEqual({ r: 255, g: 128, b: 0, w: 64 });
    expect(parseHex('#FF80')).toBeNull();
    expect(parseHex('FF8000')).toBeNull();
    expect(formatHex({ r: 255, g: 128, b: 0, w: 0 })).toBe('#FF8000');
    expect(formatHex({ r: 255, g: 128, b: 0, w: 0 }, true)).toBe('#FF800000');      // keeps the 8-digit form
    expect(formatHex({ r: 1, g: 2, b: 3, w: 4 })).toBe('#01020304');
    expect(formatHex({ r: 300, g: -5, b: 12.6, w: 0 })).toBe('#FF000D');
  });
  it('keeps 16 favourites, ignores junk, fills the first free slot', () => {
    const s = mem({ 'ktsn-led.ide.favorites': JSON.stringify(['#FF0000', 'nope', null, '#00FF0080']) });
    const f = loadFavorites(s);
    expect(f).toHaveLength(FAVORITE_SLOTS);
    expect(f.slice(0, 4)).toEqual(['#FF0000', null, null, '#00FF0080']);
    const g = addFavorite(f, '#0000FF');
    expect(g[1]).toBe('#0000FF');
    expect(addFavorite(g, '#0000FF')).toBe(g);                                      // already there
    expect(addFavorite(g, '#123456', 0)[0]).toBe('#123456');                         // replace a slot
    expect(addFavorite(Array(FAVORITE_SLOTS).fill('#000000'), '#FFFFFF')).toHaveLength(FAVORITE_SLOTS);
    expect(loadFavorites(mem())).toEqual(Array(FAVORITE_SLOTS).fill(null));
  });
});
