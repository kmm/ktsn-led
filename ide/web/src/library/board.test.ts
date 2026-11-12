import { describe, expect, it } from 'vitest';
import { ApiError } from '../devices/types';
import {
  boardEntries, boardFolder, entriesAsFile, fetchLibraryUrl, planUpload, readBoardLibrary, runUpload, uploadState, uploadSummary,
} from './board';
import { applyImport, createProgram, emptyLibrary, exportLibrary, type Library, planImport } from './library';

let n = 0;
function lib(...progs: [string, string, string][]): Library {
  let l = emptyLibrary();
  for (const [folder, name, source] of progs) l = createProgram(l, folder, name, source, 1, `p${++n}`).lib;
  return l;
}
const noErr = () => null;

describe('upload to a board', () => {
  it('plans: board names, compile errors, duplicate names across folders', () => {
    const l = lib(['a', 'wave', '(w)'], ['b', 'wave', '(w2)'], ['', 'clock', '(c)'], ['', 'bad', '(nope']);
    const { items, duplicates } = planUpload(l.programs, ['clock'], (s) => (s === '(nope' ? 'line 1 col 1: unclosed' : null));
    expect(duplicates).toEqual(['wave']);
    expect(items.map((i) => i.path)).toEqual(['bad', 'clock', 'a/wave', 'b/wave']);
    expect(items.find((i) => i.name === 'bad')!.broken).toMatch(/unclosed/);
    expect(items.find((i) => i.name === 'clock')!.onBoard).toBe(true);
    expect(planUpload(l.programs.slice(2), [], noErr).duplicates).toEqual([]);
  });

  it('states and outcomes: new, same, different with overwrite / skip, broken', async () => {
    const l = lib(['', 'new1', 'A'], ['', 'same', 'B'], ['', 'diff', 'C'], ['', 'broken', 'D']);
    const { items } = planUpload(l.programs, ['same', 'diff'], (s) => (s === 'D' ? 'oops' : null));
    items.find((i) => i.name === 'same')!.boardSource = 'B';
    expect(uploadState(items.find((i) => i.name === 'diff')!)).toBe('exists');
    items.find((i) => i.name === 'diff')!.boardSource = 'old';
    expect(items.map(uploadState)).toEqual(['new', 'different', 'new', 'same']);   // broken diff new1 same

    const put: string[] = [];
    const rs = await runUpload(items, 'skip', async (name, src) => { put.push(`${name}=${src}`); });
    expect(put).toEqual(['new1=A']);
    expect(rs.map((r) => r.outcome)).toEqual(['not sent', 'skipped', 'stored', 'unchanged']);
    expect(uploadSummary(rs)).toBe('1 stored, 1 already there, 1 skipped, 1 not sent');

    put.length = 0;
    const rs2 = await runUpload(items, 'overwrite', async (name, src) => { put.push(`${name}=${src}`); });
    expect(put).toEqual(['diff=C', 'new1=A']);
    expect(rs2.map((r) => r.outcome)).toEqual(['not sent', 'replaced', 'stored', 'unchanged']);
  });

  it('a board refusal fails one program; a lost connection stops the rest', async () => {
    const l = lib(['', 'a', '1'], ['', 'b', '2'], ['', 'c', '3']);
    const { items } = planUpload(l.programs, [], noErr);
    const seen: number[] = [];
    const rs = await runUpload(items, 'skip', async (name) => { if (name === 'a') throw new ApiError(400, 'library full'); },
      (_r, done) => seen.push(done));
    expect(rs.map((r) => r.outcome)).toEqual(['failed', 'stored', 'stored']);
    expect(rs[0].message).toBe('library full');
    expect(seen).toEqual([1, 2, 3]);
    const rs2 = await runUpload(items, 'skip', async (name) => { if (name === 'b') throw new TypeError('Failed to fetch'); });
    expect(rs2.map((r) => r.outcome)).toEqual(['stored', 'failed', 'not sent']);
    expect(rs2[2].message).toMatch(/stopped after b/);
  });
});

describe('save from a board', () => {
  it('default folder from the board name', () => {
    expect(boardFolder('Wall clock')).toBe('boards/Wall clock');
    expect(boardFolder('a/b')).toBe('boards/a-b');
    expect(boardFolder('..')).toBe('boards/board');
    expect(boardFolder('x'.repeat(60))).toBe(`boards/${'x'.repeat(40)}`);
  });

  it('entries: library programs, then running channels with a program as ch<N>', () => {
    const es = boardEntries([{ name: 'wave', src: 'W' }, { name: 'ch1', src: 'L' }],
      [{ running: true, prog: 'P0' }, { running: true, prog: 'P1' }, { running: false, prog: 'x' }, { running: true, prog: '  ' }]);
    expect(es.map((e) => `${e.key}>${e.name}`)).toEqual(['lib:ch1>ch1', 'lib:wave>wave', 'ch:0>ch0', 'ch:1>ch1-2']);
  });

  it('goes through the import plan (conflict modes apply)', () => {
    const l = lib(['boards/b1', 'wave', 'old']);
    const es = boardEntries([{ name: 'wave', src: 'new' }], [{ running: true, prog: 'P0' }]);
    const plan = planImport(l, entriesAsFile(es), 'boards/b1');
    expect(plan.items.map((i) => !!i.existing)).toEqual([true, false]);
    const r = applyImport(l, plan, 'keep', 2, () => 'x' + ++n);
    expect(r.lib.programs.map((p) => `${p.folder}/${p.name}=${p.source}`).sort())
      .toEqual(['boards/b1/ch0=P0', 'boards/b1/wave-2=new', 'boards/b1/wave=old']);
  });

  it('reads the board library with code; no library = empty', async () => {
    const t = { libList: async () => ({ programs: [{ name: 'a' }, { name: 'b' }] }), libGet: async (name: string) => `src ${name}` };
    expect(await readBoardLibrary(t)).toEqual({ programs: [{ name: 'a', src: 'src a' }, { name: 'b', src: 'src b' }], none: false });
    const old = { libList: async () => { throw new ApiError(404, 'not found'); }, libGet: async () => '' };
    expect(await readBoardLibrary(old)).toEqual({ programs: [], none: true });
    const down = { libList: async () => { throw new TypeError('Failed to fetch'); }, libGet: async () => '' };
    await expect(readBoardLibrary(down)).rejects.toThrow('Failed to fetch');
  });
});

describe('import from a URL', () => {
  const file = JSON.stringify(exportLibrary(lib(['x', 'wave', '(w)']), null));
  const respond = (body: string, init: ResponseInit = {}) => (async () => new Response(body, init)) as unknown as typeof fetch;

  it('fetches and checks the file', async () => {
    let asked: [string, RequestInit | undefined] | null = null;
    const f = (async (u: string, o?: RequestInit) => { asked = [u, o]; return new Response(file); }) as unknown as typeof fetch;
    expect(await fetchLibraryUrl(' https://example.com/lib.json ', f)).toBe(file);
    expect(asked![0]).toBe('https://example.com/lib.json');
    expect(asked![1]).toMatchObject({ credentials: 'omit' });
  });

  it('explains what went wrong', async () => {
    await expect(fetchLibraryUrl('example.com/x', respond(file))).rejects.toThrow(/not a web address/);
    await expect(fetchLibraryUrl('file:///etc/passwd', respond(file))).rejects.toThrow(/only http/);
    await expect(fetchLibraryUrl('https://e.com/x', respond('nope', { status: 404, statusText: 'Not Found' }))).rejects.toThrow('e.com answered 404 Not Found');
    await expect(fetchLibraryUrl('https://e.com/x', respond('{"a":1}'))).rejects.toThrow(/not a library file/);
    await expect(fetchLibraryUrl('https://e.com/x', (async () => { throw new TypeError('Failed to fetch'); }) as unknown as typeof fetch))
      .rejects.toThrow(/CORS/);
    await expect(fetchLibraryUrl('https://e.com/x', respond(file, { headers: { 'content-length': String(20 * 1048576) } }))).rejects.toThrow(/at most 8 MB/);
  });
});
