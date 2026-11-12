import { describe, expect, it } from 'vitest';
import {
  addFolder, applyImport, buildTree, createProgram, deleteFolder, deletePrograms, emptyLibrary, exportFileName, exportLibrary,
  folderStats, LIBRARY_KEY, type Library, loadLibrary, movePrograms, normFolder, parseLibraryFile, planImport, renameFolder,
  renameProgram, saveLibrary, saveSource, search, SOURCE_MAX, uniqueName,
} from './library';

let n = 0;
const id = () => `p${++n}`;
function lib(...progs: [string, string, string][]): Library {
  let l = emptyLibrary();
  for (const [folder, name, source] of progs) l = createProgram(l, folder, name, source, 1, id()).lib;
  return l;
}
const paths = (l: Library) => l.programs.map((p) => `${p.folder}|${p.name}`).sort();

function memStorage(init: Record<string, string> = {}) {
  const m = new Map(Object.entries(init));
  return { m, getItem: (k: string) => m.get(k) ?? null, setItem: (k: string, v: string) => { m.set(k, v); } };
}

describe('local library: folders', () => {
  it('normalises paths and rejects bad parts', () => {
    expect(normFolder(' clocks / wall/ ')).toBe('clocks/wall');
    expect(normFolder('//')).toBe('');
    expect(() => normFolder('a/../b')).toThrow(/isn't a folder name/);
    expect(() => normFolder('x'.repeat(41))).toThrow(/at most 40/);
    expect(() => normFolder('a/b/c/d/e/f/g/h/i')).toThrow(/8 deep/);
    expect(() => normFolder('a\tb')).toThrow(/control/);
  });
  it('adds folders with their parents, sorted, empty ones kept', () => {
    let l = addFolder(emptyLibrary(), 'b/c');
    l = addFolder(l, 'a');
    expect(l.folders).toEqual(['a', 'b', 'b/c']);
    expect(() => addFolder(l, 'b')).toThrow(/already exists/);
    expect(() => addFolder(l, ' / ')).toThrow(/name/);
  });
  it('renames and moves a folder with everything inside it', () => {
    let l = lib(['clocks', 'wall', 'w'], ['clocks/old', 'desk', 'd'], ['clocks-x', 'other', 'o']);
    l = renameFolder(l, 'clocks', 'displays/clocks');
    expect(l.folders).toEqual(['clocks-x', 'displays', 'displays/clocks', 'displays/clocks/old']);
    expect(paths(l)).toEqual(['clocks-x|other', 'displays/clocks/old|desk', 'displays/clocks|wall']);  // clocks-x untouched
    expect(() => renameFolder(l, 'displays', 'displays/clocks/x')).toThrow(/into itself/);
    expect(() => renameFolder(l, 'clocks-x', 'displays')).toThrow(/already exists/);
    expect(() => renameFolder(l, 'nope', 'x')).toThrow(/no folder/);
    expect(renameFolder(l, 'displays', 'displays')).toBe(l);
  });
  it('counts and deletes a folder subtree', () => {
    const l = lib(['a', 'one', '1'], ['a/b', 'two', '2'], ['c', 'three', '3']);
    expect(folderStats(l, 'a')).toEqual({ programs: 2, folders: 1 });
    const d = deleteFolder(l, 'a');
    expect(d.folders).toEqual(['c']);
    expect(paths(d)).toEqual(['c|three']);
    expect(() => deleteFolder(l, '')).toThrow();
  });
});

describe('local library: programs', () => {
  it('creates, with board names unique per folder and the board size limit', () => {
    const l = lib(['', 'clock', 'x'], ['a', 'clock', 'y']);              // same name in different folders is fine
    expect(paths(l)).toEqual(['a|clock', '|clock']);
    expect(() => createProgram(l, 'a', 'clock', '')).toThrow(/a\/clock already exists/);
    expect(() => createProgram(l, '', 'Clock', '')).toThrow(/isn't a valid name/);
    expect(() => createProgram(l, '', 'x'.repeat(16), '')).toThrow(/valid name/);
    expect(() => createProgram(l, '', 'big', 'x'.repeat(SOURCE_MAX + 1))).toThrow(/at most 3999/);
    expect(createProgram(l, 'new/deep', 'z', '').lib.folders).toEqual(['a', 'new', 'new/deep']);
  });
  it('saves, renames, moves and deletes', () => {
    let l = lib(['', 'one', '1'], ['', 'two', '2'], ['x', 'one', 'x1']);
    const [one, two, x1] = l.programs;
    l = saveSource(l, one.id, 'new', 5);
    expect(l.programs[0]).toMatchObject({ source: 'new', modified: 5 });
    expect(() => renameProgram(l, one.id, 'two')).toThrow(/already exists/);
    l = renameProgram(l, one.id, 'uno');
    expect(() => movePrograms(l, [x1.id], '')).not.toThrow();            // "one" was renamed, so no clash now
    expect(() => movePrograms(l, [two.id, x1.id], 'y')).not.toThrow();
    const l2 = renameProgram(l, x1.id, 'two');
    expect(() => movePrograms(l2, [two.id, x1.id], 'z')).toThrow(/two programs named two/);    // clash among the moved
    expect(() => movePrograms(l2, [x1.id], '')).toThrow(/the top level would have two programs named two/);
    l = movePrograms(l, [two.id], 'y/z');
    expect(l.folders).toContain('y/z');
    expect(programPathOf(l, two.id)).toBe('y/z|two');
    l = deletePrograms(l, [two.id]);
    expect(l.programs).toHaveLength(2);
    expect(() => saveSource(l, two.id, '')).toThrow(/no longer/);
  });
  it('finds a free name within 15 characters', () => {
    expect(uniqueName('wall', new Set())).toBe('wall');
    expect(uniqueName('wall', new Set(['wall', 'wall-2']))).toBe('wall-3');
    expect(uniqueName('abcdefghijklmno', new Set(['abcdefghijklmno']))).toBe('abcdefghijklm-2');
  });
  it('builds a sorted tree', () => {
    const l = addFolder(lib(['b', 'zed', ''], ['b', 'alpha', ''], ['', 'top', ''], ['a/c', 'deep', '']), 'empty');
    const t = buildTree(l);
    expect(t.programs.map((p) => p.name)).toEqual(['top']);
    expect(t.folders.map((f) => f.name)).toEqual(['a', 'b', 'empty']);
    expect(t.folders[0].folders[0]).toMatchObject({ path: 'a/c', name: 'c' });
    expect(t.folders[1].programs.map((p) => p.name)).toEqual(['alpha', 'zed']);
  });
});

function programPathOf(l: Library, pid: string) { const p = l.programs.find((q) => q.id === pid)!; return `${p.folder}|${p.name}`; }

describe('local library: search', () => {
  const l = lib(['clocks', 'wall', '(seg7 (buf X) 1)\n(fill (buf O) #FF0000)'], ['', 'fire', '; FLAMES\n(fill-random (buf O))'],
    ['fx', 'sparkle', '(fill-pick (buf Z) #000000 #FFFFFF 0.01)']);
  it('matches name, folder or code, ignoring case, with the first matching line', () => {
    expect(search(l, '  ')).toEqual([]);
    expect(search(l, 'WALL').map((h) => [h.program.name, h.in])).toEqual([['wall', 'name']]);
    expect(search(l, 'clock').map((h) => [h.program.name, h.in])).toEqual([['wall', 'folder']]);
    const h = search(l, 'flames');
    expect(h).toHaveLength(1);
    expect(h[0]).toMatchObject({ in: 'code', line: '; FLAMES', lineNo: 1 });
    expect(search(l, '(fill').map((x) => [x.program.name, x.lineNo])).toEqual([['fire', 2], ['wall', 2], ['sparkle', 1]]);
  });
});

describe('local library: export and import', () => {
  it('round-trips everything, empty folders included', () => {
    const l = addFolder(lib(['a', 'one', '1'], ['', 'two', '2']), 'empty');
    const f = exportLibrary(l, null, new Date('2026-11-04T10:00:00Z'));
    expect(f).toMatchObject({ format: 'ktsn-led-library', version: 1, exported: '2026-11-04T10:00:00.000Z', folders: ['a', 'empty'] });
    const parsed = parseLibraryFile(JSON.stringify(f));
    const r = applyImport(emptyLibrary(), planImport(emptyLibrary(), parsed, ''), 'skip', 1, id);
    expect(r).toMatchObject({ added: 2, replaced: 0, renamed: 0, skipped: 0, unchanged: 0 });
    expect(r.lib.folders).toEqual(['a', 'empty']);
    expect(paths(r.lib)).toEqual(paths(l));
    expect(exportFileName(new Date(2026, 0, 5))).toBe('ktsn-led-library-2026-01-05.json');
  });
  it('exports a selection with just its folders', () => {
    const l = addFolder(lib(['a/b', 'one', '1'], ['c', 'two', '2']), 'empty');
    const f = exportLibrary(l, [l.programs[0].id]);
    expect(f.folders).toEqual(['a', 'a/b']);
    expect(f.programs).toEqual([{ folder: 'a/b', name: 'one', source: '1' }]);
  });
  it('rejects bad files, saying what and where', () => {
    const file = (o: object) => JSON.stringify({ format: 'ktsn-led-library', version: 1, programs: [], ...o });
    expect(parseLibraryFile(JSON.stringify({ format: 'ledctl-library', version: 1, programs: [] })).programs).toEqual([]);   // before 0.18.0
    expect(() => parseLibraryFile('nope')).toThrow(/not JSON/);
    expect(() => parseLibraryFile('{"programs": []}')).toThrow(/not a library file/);
    expect(() => parseLibraryFile(file({ version: 2 }))).toThrow(/newer IDE/);
    expect(() => parseLibraryFile(file({ programs: 'x' }))).toThrow(/programs list/);
    expect(() => parseLibraryFile(file({ programs: [{ name: 'Bad', source: '' }] }))).toThrow(/program 1 \(Bad\): not a valid program name/);
    expect(() => parseLibraryFile(file({ programs: [{ name: 'ok' }] }))).toThrow(/no source/);
    expect(() => parseLibraryFile(file({ programs: [{ name: 'ok', source: 'x'.repeat(4000) }] }))).toThrow(/longer than/);
    expect(() => parseLibraryFile(file({ programs: [{ name: 'ok', source: '', folder: '../x' }] }))).toThrow(/isn't a folder name/);
    expect(() => parseLibraryFile(file({ programs: [{ name: 'ok', source: '' }, { name: 'ok', source: '', folder: '/' }] }))).toThrow(/ok is in it twice/);
    expect(() => parseLibraryFile(file({ folders: [3] }))).toThrow(/folder 1 isn't text/);
    expect(parseLibraryFile(file({ programs: [{ name: 'ok', source: 's', extra: 1 }] })).programs).toEqual([{ folder: '', name: 'ok', source: 's' }]);
  });
  it('imports into a folder, with overwrite / skip / keep both for names already there', () => {
    const l = lib(['in/a', 'same', 'S'], ['in/a', 'diff', 'old']);
    const file = parseLibraryFile(JSON.stringify({ format: 'ktsn-led-library', version: 1, folders: ['a', 'e'],
      programs: [{ folder: 'a', name: 'same', source: 'S' }, { folder: 'a', name: 'diff', source: 'new' }, { folder: 'a', name: 'fresh', source: 'f' }] }));
    const plan = planImport(l, file, 'in');
    expect(plan.items.map((i) => [i.folder, i.name, !!i.existing])).toEqual([['in/a', 'same', true], ['in/a', 'diff', true], ['in/a', 'fresh', false]]);
    const over = applyImport(l, plan, 'overwrite', 9, id);
    expect(over).toMatchObject({ added: 1, replaced: 1, unchanged: 1, skipped: 0, renamed: 0 });
    expect(over.lib.programs.find((p) => p.name === 'diff')).toMatchObject({ source: 'new', modified: 9 });
    expect(over.lib.folders).toEqual(['in', 'in/a', 'in/e']);
    const skip = applyImport(l, plan, 'skip', 9, id);
    expect(skip).toMatchObject({ added: 1, skipped: 1, unchanged: 1 });
    expect(skip.lib.programs.find((p) => p.name === 'diff')!.source).toBe('old');
    const keep = applyImport(l, plan, 'keep', 9, id);
    expect(keep).toMatchObject({ added: 1, renamed: 1, unchanged: 1 });
    expect(paths(keep.lib)).toEqual(['in/a|diff', 'in/a|diff-2', 'in/a|fresh', 'in/a|same']);
    expect(() => applyImport(l, planImport(l, parseLibraryFile(JSON.stringify({ format: 'ktsn-led-library', version: 1,
      programs: [{ folder: 'a/b/c/d/e/f/g', name: 'x', source: '' }] })), 'in/deep'), 'skip')).toThrow(/8 deep/);
  });
});

describe('local library: storage', () => {
  it('saves and loads', () => {
    const s = memStorage();
    const l = addFolder(lib(['a', 'one', '1']), 'b');
    expect(saveLibrary(s, l)).toBeNull();
    expect(loadLibrary(s)).toEqual({ lib: l });
    expect(loadLibrary(memStorage())).toEqual({ lib: emptyLibrary() });
  });
  it('keeps unreadable data aside instead of overwriting it', () => {
    const s = memStorage({ [LIBRARY_KEY]: '{"broken' });
    const r = loadLibrary(s);
    expect(r.lib).toEqual(emptyLibrary());
    expect(r.problem).toMatch(/unreadable/);
    expect(s.m.get(LIBRARY_KEY + '.unreadable')).toBe('{"broken');
  });
  it('reports a full storage', () => {
    const full = { setItem: () => { throw Object.assign(new Error('full'), { name: 'QuotaExceededError' }); } };
    expect(saveLibrary(full, emptyLibrary())).toMatch(/QuotaExceededError/);
  });
});
