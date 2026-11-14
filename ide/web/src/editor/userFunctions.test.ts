import { describe, expect, it } from 'vitest';
import { libraryDefs, userFunctions } from './dslLanguage';

describe('user functions in the editor', () => {
  it('finds defns with their parameters, line and comment', () => {
    const src = '(fill (buf O) #000000)\n; darker by k\n;; (0..1)\n(defn dim (c:color   k:decimal) (lerp #000000 c k))\n' +
      '(def x 1) (defn one () 1)\n(defn  bad';
    expect(userFunctions(src)).toEqual([
      { name: 'dim', params: 'c:color k:decimal', line: 4, doc: 'darker by k (0..1)' },
      { name: 'one', params: '', line: 5, doc: '' },          // not at the start of its line: no doc comment
    ]);
  });
  it('lists the names used libraries define, through other libraries too', () => {
    const uses = new Map<string, string | { why: string }>([
      ['pal', '(use base) (def fire (set (slice (buf G0) 0 3) 0 #FF0000 #FF8000 #FFFF00))\n(def pals (list fire))'],
      ['base', '(def warm #FF8800)'], ['gone', { why: 'no such program' }],
    ]);
    expect(libraryDefs('(use pal) (use gone)', uses)).toEqual([
      { name: 'fire', lib: 'pal' }, { name: 'pals', lib: 'pal' }, { name: 'warm', lib: 'base' },
    ]);
    expect(libraryDefs('(def x 1)', uses)).toEqual([]);
  });
});
