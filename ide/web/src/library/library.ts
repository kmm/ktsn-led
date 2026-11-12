// The IDE's local library: named channel programs kept in the browser (localStorage), organised in folders.
// Names follow the board library's rules (LIB_NAME) and are unique within a folder, so a program uploads to a board
// under the same name. Folders are paths ("clocks/wall"); `folders` lists every folder (ancestors included, empty
// ones too); the root is "" and is not listed. Pure functions; the UI lives in LibraryPanel / App.
// Operations return a new Library and throw an Error whose message is meant for the user.

import { LIB_NAME } from '../tabs/tabs';

export const LIBRARY_KEY = 'ktsn-led.ide.library';
export const SOURCE_MAX = 3999;            // the board's limit (main/settings.h SOURCE_MAX)
export const FOLDER_PART_MAX = 40;
export const FOLDER_DEPTH_MAX = 8;
export const IMPORT_MAX = 5000;
export const FILE_FORMAT = 'ktsn-led-library';
export const FILE_VERSION = 1;

export interface LocalProgram { id: string; folder: string; name: string; source: string; modified: number }
export interface Library { folders: string[]; programs: LocalProgram[] }

export const emptyLibrary = (): Library => ({ folders: [], programs: [] });
export const newProgramId = () => Math.random().toString(36).slice(2, 10) + Date.now().toString(36).slice(-4);

/* ---- folders ---- */

/** A folder path in its canonical form: parts trimmed, empty parts dropped. Throws on a bad part. */
export function normFolder(path: string): string {
  const parts = path.split('/').map((p) => p.trim()).filter((p) => p !== '');
  if (parts.length > FOLDER_DEPTH_MAX) throw new Error(`folders nest at most ${FOLDER_DEPTH_MAX} deep`);
  for (const p of parts) {
    if (p === '.' || p === '..') throw new Error(`"${p}" isn't a folder name`);
    if (p.length > FOLDER_PART_MAX) throw new Error(`folder names are at most ${FOLDER_PART_MAX} characters`);
    if (/[\u0000-\u001f\u007f]/.test(p)) throw new Error('folder names can\'t contain control characters');
  }
  return parts.join('/');
}

export const joinFolder = (a: string, b: string) => (a && b ? `${a}/${b}` : a || b);
export const folderName = (f: string) => f.slice(f.lastIndexOf('/') + 1);
export const parentFolder = (f: string) => (f.includes('/') ? f.slice(0, f.lastIndexOf('/')) : '');
/** f is root itself or inside it. */
export const inFolder = (f: string, root: string) => root === '' || f === root || f.startsWith(root + '/');
/** Where to show a program: "clocks/wall/name", or just the name at the root. */
export const programPath = (p: { folder: string; name: string }) => joinFolder(p.folder, p.name);

function compareFolders(a: string, b: string): number {
  const x = a.split('/'), y = b.split('/');
  for (let i = 0; i < Math.min(x.length, y.length); i++) {
    const c = x[i].localeCompare(y[i], undefined, { sensitivity: 'base' }) || (x[i] < y[i] ? -1 : x[i] > y[i] ? 1 : 0);
    if (c) return c;
  }
  return x.length - y.length;
}

/** The folder list with `more` (and all their ancestors) added, sorted, without duplicates or the root. */
function withFolders(folders: string[], more: string[]): string[] {
  const set = new Set(folders);
  for (const f of more) for (let g = f; g !== ''; g = parentFolder(g)) set.add(g);
  return [...set].sort(compareFolders);
}

export function addFolder(lib: Library, path: string): Library {
  const f = normFolder(path);
  if (f === '') throw new Error('give the folder a name');
  if (lib.folders.includes(f)) throw new Error(`folder ${f} already exists`);
  return { ...lib, folders: withFolders(lib.folders, [f]) };
}

/** Rename or move a folder with everything in it. The new path must not exist yet. */
export function renameFolder(lib: Library, from: string, to: string): Library {
  const t = normFolder(to);
  if (!lib.folders.includes(from)) throw new Error(`no folder ${from}`);
  if (t === '') throw new Error('give the folder a name');
  if (t === from) return lib;
  if (inFolder(t, from)) throw new Error('a folder can\'t move into itself');
  if (lib.folders.includes(t)) throw new Error(`folder ${t} already exists`);
  const move = (f: string) => (inFolder(f, from) ? t + f.slice(from.length) : f);
  return {
    folders: withFolders(lib.folders.filter((f) => !inFolder(f, from)), lib.folders.filter((f) => inFolder(f, from)).map(move)),
    programs: lib.programs.map((p) => (inFolder(p.folder, from) ? { ...p, folder: move(p.folder) } : p)),
  };
}

/** What a folder holds, all levels down (for the delete question). */
export function folderStats(lib: Library, path: string): { programs: number; folders: number } {
  return {
    programs: lib.programs.filter((p) => inFolder(p.folder, path)).length,
    folders: lib.folders.filter((f) => f !== path && inFolder(f, path)).length,
  };
}

/** Delete a folder and everything in it. */
export function deleteFolder(lib: Library, path: string): Library {
  if (path === '') throw new Error('the top level can\'t be deleted');
  return { folders: lib.folders.filter((f) => !inFolder(f, path)), programs: lib.programs.filter((p) => !inFolder(p.folder, path)) };
}

/* ---- programs ---- */

export function checkName(name: string): void {
  if (!LIB_NAME.test(name)) throw new Error(`"${name}" isn't a valid name: a-z 0-9 - _, starting with a letter, up to 15 characters`);
}
function checkSource(src: string): void {
  if (src.length > SOURCE_MAX) throw new Error(`the program is ${src.length} characters; at most ${SOURCE_MAX} fit on a board`);
}

export const findProgram = (lib: Library, folder: string, name: string) => lib.programs.find((p) => p.folder === folder && p.name === name);
export const programById = (lib: Library, id: string) => lib.programs.find((p) => p.id === id);

export function createProgram(lib: Library, folder: string, name: string, source: string, now = Date.now(), id = newProgramId()): { lib: Library; program: LocalProgram } {
  const f = normFolder(folder);
  checkName(name);
  checkSource(source);
  if (findProgram(lib, f, name)) throw new Error(`${programPath({ folder: f, name })} already exists`);
  const program: LocalProgram = { id, folder: f, name, source, modified: now };
  return { lib: { folders: withFolders(lib.folders, [f]), programs: [...lib.programs, program] }, program };
}

export function saveSource(lib: Library, id: string, source: string, now = Date.now()): Library {
  checkSource(source);
  if (!programById(lib, id)) throw new Error('that program is no longer in the library');
  return { ...lib, programs: lib.programs.map((p) => (p.id === id ? { ...p, source, modified: now } : p)) };
}

export function renameProgram(lib: Library, id: string, name: string): Library {
  const p = programById(lib, id);
  if (!p) throw new Error('that program is no longer in the library');
  checkName(name);
  if (name === p.name) return lib;
  if (findProgram(lib, p.folder, name)) throw new Error(`${programPath({ folder: p.folder, name })} already exists`);
  return { ...lib, programs: lib.programs.map((q) => (q.id === id ? { ...q, name } : q)) };
}

/** Move programs into a folder (created if needed). Refused as a whole if any name would clash there. */
export function movePrograms(lib: Library, ids: string[], folder: string): Library {
  const f = normFolder(folder), moving = new Set(ids);
  const taken = new Set(lib.programs.filter((p) => p.folder === f && !moving.has(p.id)).map((p) => p.name));
  const clash: string[] = [];
  for (const p of lib.programs) if (moving.has(p.id)) { if (taken.has(p.name)) clash.push(p.name); taken.add(p.name); }
  if (clash.length) throw new Error(`${f || 'the top level'} would have two programs named ${[...new Set(clash)].join(', ')}`);
  return { folders: withFolders(lib.folders, [f]), programs: lib.programs.map((p) => (moving.has(p.id) ? { ...p, folder: f } : p)) };
}

export function deletePrograms(lib: Library, ids: string[]): Library {
  const del = new Set(ids);
  return { ...lib, programs: lib.programs.filter((p) => !del.has(p.id)) };
}

/** `base`, or base-2, base-3 ... (cut to fit 15 characters) if that name is taken. */
export function uniqueName(base: string, taken: Set<string>): string {
  if (!taken.has(base)) return base;
  for (let n = 2; ; n++) {
    const suffix = `-${n}`, name = base.slice(0, 15 - suffix.length) + suffix;
    if (!taken.has(name)) return name;
  }
}

/* ---- tree and search ---- */

export interface FolderNode { path: string; name: string; folders: FolderNode[]; programs: LocalProgram[] }

export function buildTree(lib: Library): FolderNode {
  const root: FolderNode = { path: '', name: '', folders: [], programs: [] };
  const nodes = new Map<string, FolderNode>([['', root]]);
  for (const f of lib.folders) {                          // sorted, so parents come first
    const n: FolderNode = { path: f, name: folderName(f), folders: [], programs: [] };
    nodes.set(f, n);
    nodes.get(parentFolder(f))?.folders.push(n);
  }
  for (const p of lib.programs) nodes.get(p.folder)?.programs.push(p);
  for (const n of nodes.values()) n.programs.sort((a, b) => a.name.localeCompare(b.name));
  return root;
}

export interface SearchHit { program: LocalProgram; in: 'name' | 'folder' | 'code'; line?: string; lineNo?: number }

/** Programs whose name, folder or code contains `query` (ignoring case), sorted by path. Code hits give the first
 *  matching line. */
export function search(lib: Library, query: string): SearchHit[] {
  const q = query.trim().toLowerCase();
  if (!q) return [];
  const hits: SearchHit[] = [];
  for (const p of lib.programs) {
    if (p.name.toLowerCase().includes(q)) { hits.push({ program: p, in: 'name' }); continue; }
    if (p.folder.toLowerCase().includes(q)) { hits.push({ program: p, in: 'folder' }); continue; }
    const lines = p.source.split('\n');
    const i = lines.findIndex((l) => l.toLowerCase().includes(q));
    if (i >= 0) hits.push({ program: p, in: 'code', line: lines[i].trim(), lineNo: i + 1 });
  }
  return hits.sort((a, b) => compareFolders(a.program.folder, b.program.folder) || a.program.name.localeCompare(b.program.name));
}

/* ---- export / import ---- */

export interface LibraryFile {
  format: typeof FILE_FORMAT;
  version: typeof FILE_VERSION;
  exported: string;
  folders: string[];
  programs: { folder: string; name: string; source: string }[];
}

/** The export file for all programs (ids null: empty folders too) or the chosen ones. */
export function exportLibrary(lib: Library, ids: string[] | null, now = new Date()): LibraryFile {
  const pick = ids ? new Set(ids) : null;
  const progs = lib.programs.filter((p) => !pick || pick.has(p.id))
    .sort((a, b) => compareFolders(a.folder, b.folder) || a.name.localeCompare(b.name));
  return {
    format: FILE_FORMAT, version: FILE_VERSION, exported: now.toISOString(),
    folders: pick ? withFolders([], progs.map((p) => p.folder)) : [...lib.folders],
    programs: progs.map(({ folder, name, source }) => ({ folder, name, source })),
  };
}

export const exportFileName = (now = new Date()) =>
  `ktsn-led-library-${now.getFullYear()}-${String(now.getMonth() + 1).padStart(2, '0')}-${String(now.getDate()).padStart(2, '0')}.json`;

/** Read an export file. Throws (with what is wrong and where) unless the whole file is valid. */
export function parseLibraryFile(text: string): { folders: string[]; programs: { folder: string; name: string; source: string }[] } {
  let v: unknown;
  try { v = JSON.parse(text); } catch { throw new Error('not a library file (not JSON)'); }
  const o = v as Partial<LibraryFile> | null;
  if (!o || typeof o !== 'object' || (o.format !== FILE_FORMAT && o.format !== 'ledctl-library')) throw new Error('not a library file (exported from the KTSN LED IDE)');
  if (typeof o.version !== 'number' || o.version < 1) throw new Error('library file without a valid version');
  if (o.version > FILE_VERSION) throw new Error(`library file version ${o.version} is from a newer IDE (this one reads version ${FILE_VERSION})`);
  if (!Array.isArray(o.programs)) throw new Error('library file without a programs list');
  if (o.folders !== undefined && !Array.isArray(o.folders)) throw new Error('library file: folders must be a list');
  if (o.programs.length > IMPORT_MAX) throw new Error(`library file has ${o.programs.length} programs; at most ${IMPORT_MAX} at once`);
  const folders = (o.folders ?? []).map((f, i) => {
    if (typeof f !== 'string') throw new Error(`library file: folder ${i + 1} isn't text`);
    try { return normFolder(f); } catch (e) { throw new Error(`library file: folder "${f}": ${(e as Error).message}`); }
  }).filter((f) => f !== '');
  const seen = new Set<string>();
  const programs = o.programs.map((p, i) => {
    const at = `program ${i + 1}${typeof p?.name === 'string' ? ` (${p.name})` : ''}`;
    if (!p || typeof p !== 'object') throw new Error(`library file: ${at} isn't an object`);
    if (typeof p.name !== 'string' || !LIB_NAME.test(p.name)) throw new Error(`library file: ${at}: not a valid program name`);
    if (typeof p.source !== 'string') throw new Error(`library file: ${at}: no source`);
    if (p.source.length > SOURCE_MAX) throw new Error(`library file: ${at}: longer than ${SOURCE_MAX} characters`);
    if (p.folder !== undefined && typeof p.folder !== 'string') throw new Error(`library file: ${at}: folder isn't text`);
    let folder: string;
    try { folder = normFolder(p.folder ?? ''); } catch (e) { throw new Error(`library file: ${at}: ${(e as Error).message}`); }
    const key = programPath({ folder, name: p.name });
    if (seen.has(key)) throw new Error(`library file: ${key} is in it twice`);
    seen.add(key);
    return { folder, name: p.name, source: p.source };
  });
  return { folders, programs };
}

export interface ImportItem { folder: string; name: string; source: string; existing?: LocalProgram }
export type ImportMode = 'overwrite' | 'skip' | 'keep';

/** The file's programs placed under `target`, each with the program it would replace (if any). */
export function planImport(lib: Library, file: ReturnType<typeof parseLibraryFile>, target: string): { items: ImportItem[]; folders: string[] } {
  const t = normFolder(target);
  return {
    folders: file.folders.map((f) => joinFolder(t, f)),
    items: file.programs.map((p) => {
      const folder = joinFolder(t, p.folder);
      return { folder, name: p.name, source: p.source, existing: findProgram(lib, folder, p.name) };
    }),
  };
}

export interface ImportResult { lib: Library; added: number; replaced: number; renamed: number; skipped: number; unchanged: number }

/** Apply a planned import. Programs identical to the one already there count as unchanged whatever the mode. */
export function applyImport(lib: Library, plan: ReturnType<typeof planImport>, mode: ImportMode, now = Date.now(), newId = newProgramId): ImportResult {
  if (plan.items.some((i) => normFolder(i.folder).split('/').length > FOLDER_DEPTH_MAX)) throw new Error(`folders nest at most ${FOLDER_DEPTH_MAX} deep`);
  const r: ImportResult = { lib, added: 0, replaced: 0, renamed: 0, skipped: 0, unchanged: 0 };
  const programs = [...lib.programs];
  for (const it of plan.items) {
    const existing = programs.find((p) => p.folder === it.folder && p.name === it.name);
    if (existing && existing.source === it.source) { r.unchanged++; continue; }
    if (existing && mode === 'skip') { r.skipped++; continue; }
    if (existing && mode === 'overwrite') {
      programs[programs.indexOf(existing)] = { ...existing, source: it.source, modified: now };
      r.replaced++;
      continue;
    }
    let name = it.name;
    if (existing) { name = uniqueName(it.name, new Set(programs.filter((p) => p.folder === it.folder).map((p) => p.name))); r.renamed++; }
    else r.added++;
    programs.push({ id: newId(), folder: it.folder, name, source: it.source, modified: now });
  }
  r.lib = { folders: withFolders(lib.folders, [...plan.folders, ...plan.items.map((i) => i.folder)]), programs };
  return r;
}

/* ---- storage ---- */

function validLibrary(v: unknown): v is Library {
  const o = v as Library;
  return !!o && Array.isArray(o.folders) && o.folders.every((f) => typeof f === 'string') && Array.isArray(o.programs) &&
    o.programs.every((p) => !!p && typeof p.id === 'string' && typeof p.folder === 'string' && typeof p.name === 'string' &&
      typeof p.source === 'string' && typeof p.modified === 'number');
}

/** The saved library. Something unreadable is kept under `<key>.unreadable` (never silently overwritten) and an empty
 *  library is returned with a message. */
export function loadLibrary(storage: Pick<Storage, 'getItem' | 'setItem'>): { lib: Library; problem?: string } {
  let raw: string | null = null;
  try { raw = storage.getItem(LIBRARY_KEY); } catch { return { lib: emptyLibrary(), problem: 'the browser\'s storage can\'t be read' }; }
  if (raw === null) return { lib: emptyLibrary() };
  try {
    const v = JSON.parse(raw) as { version?: number } & Library;
    if (v.version === 1 && validLibrary(v)) return { lib: { folders: withFolders(v.folders, v.programs.map((p) => p.folder)), programs: v.programs } };
  } catch { /* below */ }
  try { storage.setItem(LIBRARY_KEY + '.unreadable', raw); } catch { /* best effort */ }
  return { lib: emptyLibrary(), problem: `the saved library couldn't be read; its data was kept in ${LIBRARY_KEY}.unreadable` };
}

/** Store the library; returns why it failed (usually the browser's storage is full), or null. */
export function saveLibrary(storage: Pick<Storage, 'setItem'>, lib: Library): string | null {
  try { storage.setItem(LIBRARY_KEY, JSON.stringify({ version: 1, ...lib })); return null; }
  catch (e) { return `not saved: the browser's storage is full or blocked (${(e as Error).name})`; }
}

/** Approximate bytes the library takes in storage. */
export const librarySize = (lib: Library) => JSON.stringify(lib).length;
