// Moving programs between the local library and a board: upload a selection into the board's library, save the
// board's library and channel programs into the local one, and fetch a library file from a URL. Pure functions plus
// small async helpers that take the transport calls they need (so they can be tested with fakes).

import { ApiError } from '../devices/types';
import { FOLDER_PART_MAX, type LocalProgram, normFolder, parseLibraryFile, programPath, uniqueName } from './library';
import { usesFirst } from './uses';

/* ---- upload: local library -> board library ---- */

export interface UploadItem {
  id: string;
  path: string;
  name: string;
  source: string;
  /** Compile error here (not sent: the board would refuse it). */
  broken?: string;
  /** The name is already in the board's library. */
  onBoard: boolean;
  /** The board's code for that name, once read (undefined: not read / not on the board). */
  boardSource?: string;
}
export type UploadMode = 'overwrite' | 'skip';
export type UploadOutcome = 'stored' | 'replaced' | 'unchanged' | 'skipped' | 'not sent' | 'failed';
export interface UploadResult { path: string; name: string; outcome: UploadOutcome; message?: string }

/** The selection as upload items. Board names are flat, so two selected programs with one name (in different
 *  folders) can't both go: `duplicates` lists those names, and the upload is refused until one is deselected. */
export function planUpload(programs: LocalProgram[], boardNames: Iterable<string>, check: (src: string) => string | null):
  { items: UploadItem[]; duplicates: string[] } {
  const on = new Set(boardNames), count = new Map<string, number>();
  for (const p of programs) count.set(p.name, (count.get(p.name) ?? 0) + 1);
  const items = usesFirst([...programs]
    .sort((a, b) => a.name.localeCompare(b.name) || a.folder.localeCompare(b.folder))
    .map((p) => {
      const broken = check(p.source) ?? undefined;
      return { id: p.id, path: programPath(p), name: p.name, source: p.source, onBoard: on.has(p.name), ...(broken ? { broken } : {}) };
    }));
  return { items, duplicates: [...count].filter(([, n]) => n > 1).map(([name]) => name).sort() };
}

/** new / same code / different code / not read yet, for the dialog. */
export function uploadState(i: UploadItem): 'new' | 'same' | 'different' | 'exists' {
  if (!i.onBoard) return 'new';
  if (i.boardSource === undefined) return 'exists';
  return i.boardSource === i.source ? 'same' : 'different';
}

/** Send the items one by one. A board's refusal (ApiError: compile error, storage full) fails that program only; any
 *  other error (connection lost) stops the upload and marks the rest "not sent". */
export async function runUpload(items: UploadItem[], mode: UploadMode, put: (name: string, src: string) => Promise<void>,
  onResult?: (r: UploadResult, done: number) => void): Promise<UploadResult[]> {
  const out: UploadResult[] = [];
  let stopped: string | null = null;
  for (const i of items) {
    const base = { path: i.path, name: i.name };
    let r: UploadResult;
    if (stopped !== null) r = { ...base, outcome: 'not sent', message: stopped };
    else if (i.broken) r = { ...base, outcome: 'not sent', message: `doesn't compile: ${i.broken}` };
    else if (i.onBoard && i.boardSource === i.source) r = { ...base, outcome: 'unchanged' };
    else if (i.onBoard && mode === 'skip') r = { ...base, outcome: 'skipped', message: 'already on the board' };
    else {
      try {
        await put(i.name, i.source);
        r = { ...base, outcome: i.onBoard ? 'replaced' : 'stored' };
      } catch (e) {
        const msg = e instanceof Error ? e.message : String(e);
        r = { ...base, outcome: 'failed', message: msg };
        if (!(e instanceof ApiError)) stopped = `stopped after ${i.name} failed`;
      }
    }
    out.push(r);
    onResult?.(r, out.length);
  }
  return out;
}

export function uploadSummary(rs: UploadResult[]): string {
  const n = (o: UploadOutcome) => rs.filter((r) => r.outcome === o).length;
  return ([['stored', 'stored'], ['replaced', 'replaced'], ['unchanged', 'already there'], ['skipped', 'skipped'],
    ['not sent', 'not sent'], ['failed', 'failed']] as [UploadOutcome, string][])
    .filter(([o]) => n(o)).map(([o, label]) => `${n(o)} ${label}`).join(', ') || 'nothing to upload';
}

/* ---- save from a board: board library + channel programs -> local library ---- */

export interface BoardEntry { key: string; kind: 'library' | 'channel'; label: string; name: string; source: string }

/** The local folder a board's programs go to by default: boards/<board name> (made a valid folder name). */
export function boardFolder(boardName: string): string {
  const part = boardName.replace(/\//g, '-').replace(/[\u0000-\u001f\u007f]/g, '').trim().slice(0, FOLDER_PART_MAX).trim();
  const f = part === '.' || part === '..' || !part ? 'board' : part;
  return normFolder(`boards/${f}`);
}

/** What a board can give: its library programs under their names, then each running channel's program as ch<N>
 *  (ch<N>-2 ... if the library already uses that name). Channels without a program are left out. */
export function boardEntries(library: { name: string; src: string }[], channels: { running?: boolean; prog?: string | null }[]): BoardEntry[] {
  const out: BoardEntry[] = [...library].sort((a, b) => a.name.localeCompare(b.name))
    .map((p) => ({ key: `lib:${p.name}`, kind: 'library', label: p.name, name: p.name, source: p.src }));
  const taken = new Set(out.map((e) => e.name));
  channels.forEach((c, ch) => {
    if (!c?.running || !c.prog?.trim()) return;
    const name = uniqueName(`ch${ch}`, taken);
    taken.add(name);
    out.push({ key: `ch:${ch}`, kind: 'channel', label: `channel ${ch}`, name, source: c.prog });
  });
  return out;
}

/** The chosen entries in the shape planImport takes (at the target folder's top). */
export const entriesAsFile = (es: BoardEntry[]) => ({ folders: [], programs: es.map((e) => ({ folder: '', name: e.name, source: e.source })) });

/** Read the board's library with every program's code. Firmware without a library (404) gives an empty list and `none`. */
export async function readBoardLibrary(t: { libList(): Promise<{ programs: { name: string }[] }>; libGet(name: string): Promise<string> }):
  Promise<{ programs: { name: string; src: string }[]; none: boolean }> {
  let names: string[];
  try { names = (await t.libList()).programs.map((p) => p.name); }
  catch (e) { if (e instanceof ApiError && e.status === 404) return { programs: [], none: true }; throw e; }
  const programs: { name: string; src: string }[] = [];
  for (const name of names) programs.push({ name, src: await t.libGet(name) });
  return { programs, none: false };
}

/* ---- import from a URL ---- */

export const URL_IMPORT_MAX = 8 * 1024 * 1024;

/** Fetch a library file. Only http(s); checked as a library file before it is returned (same rules as a file). */
export async function fetchLibraryUrl(url: string, f: typeof fetch = fetch): Promise<string> {
  let u: URL;
  try { u = new URL(url.trim()); } catch { throw new Error('not a web address (e.g. https://example.com/library.json)'); }
  if (u.protocol !== 'https:' && u.protocol !== 'http:') throw new Error('only http:// and https:// addresses can be fetched');
  let r: Response;
  try { r = await f(u.href, { credentials: 'omit', cache: 'no-store' }); }
  catch {
    throw new Error(`couldn't fetch ${u.host}: unreachable, or the server doesn't allow other sites to read it (CORS). ` +
      'Download the file and use Choose file… instead');
  }
  if (!r.ok) throw new Error(`${u.host} answered ${r.status}${r.statusText ? ` ${r.statusText}` : ''}`);
  const len = Number(r.headers.get('content-length') ?? 0);
  if (len > URL_IMPORT_MAX) throw new Error(`the file is ${Math.round(len / 1048576)} MB; at most ${URL_IMPORT_MAX / 1048576} MB`);
  const text = await r.text();
  if (text.length > URL_IMPORT_MAX) throw new Error(`the file is larger than ${URL_IMPORT_MAX / 1048576} MB`);
  parseLibraryFile(text);                                  // throws with what is wrong
  return text;
}
