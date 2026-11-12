import { describe, expect, it } from 'vitest';
import { loadUses, localSources, usedNames, usesFirst } from './uses';

const prog = (folder: string, name: string, source: string) => ({ id: name + folder, folder, name, source, modified: 1 });

describe('use in the IDE', () => {
  it('finds used names outside comments and strings', () => {
    expect(usedNames('(use a) ; (use b)\n( use  c)(def s "(use d)") (use a)')).toEqual(['a', 'c']);
  });
  it('local sources: a name in two folders can not be used', () => {
    const m = localSources([prog('', 'a', 'A'), prog('x', 'b', 'B'), prog('y', 'b', 'B2')]);
    expect(m.get('a')).toBe('A');
    expect(m.get('b')).toEqual({ why: expect.stringMatching(/more than one folder/) });
  });
  it('loads only what is reachable, once, and skips what the board lacks', async () => {
    const srcs: Record<string, string> = { a: '(use b) (use gone)', b: '(use a)', c: 'unused', bad: '' };
    const asked: string[] = [];
    const m = await loadUses(['a', 'nope', 'bad'], ['a', 'b', 'c', 'bad'], async (n) => {
      asked.push(n);
      if (n === 'bad') throw new Error('lost');
      return srcs[n];
    });
    expect([...m.keys()].sort()).toEqual(['a', 'b']);
    expect(asked.sort()).toEqual(['a', 'b', 'bad']);
  });
  it('orders what others use first, otherwise keeps the order', () => {
    const it = (name: string, source = '') => ({ name, source });
    expect(usesFirst([it('a', '(use c)'), it('b'), it('c', '(use d)'), it('d')]).map((i) => i.name)).toEqual(['d', 'c', 'a', 'b']);
    expect(usesFirst([it('x', '(use y)'), it('y', '(use x)')]).map((i) => i.name)).toEqual(['y', 'x']);
    expect(usesFirst([it('p'), it('q')]).map((i) => i.name)).toEqual(['p', 'q']);
  });
});
