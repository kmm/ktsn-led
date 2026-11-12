import { useEffect, useMemo, useRef, useState } from 'react';
import type { Core } from '../core/ktsnLed';
import {
  addFolder, applyImport, buildTree, createProgram, deleteFolder, deletePrograms, exportFileName, exportLibrary, findProgram,
  folderName, type FolderNode, folderStats, type ImportMode, inFolder, joinFolder, type Library, librarySize, type LocalProgram,
  movePrograms, normFolder, parentFolder, parseLibraryFile, planImport, programPath, renameFolder, renameProgram, search,
  uniqueName,
} from './library';
import { fetchLibraryUrl } from './board';

const CLOSED_KEY = 'ktsn-led.ide.library.closed';
const NEW_SOURCE = (name: string) => `; ${name}\n(fill (buf O) #000000)\n`;

interface Props {
  core: Core;
  lib: Library;
  /** Why the library couldn't be read or stored, if so. */
  problem: string;
  /** Apply a change to the library and store it; returns why it failed (the change's own error, or storage), or null. */
  change(f: (l: Library) => Library): string | null;
  /** Open a program in an editor tab. */
  onOpen(p: LocalProgram): void;
  /** The connected board (for upload / save from it), or null. */
  board: { name: string } | null;
  /** Upload these programs to the connected board's library (opens the upload dialog). */
  onUpload(ids: string[]): void;
  /** Save programs from the connected board into the local library (opens its dialog). */
  onFromBoard(): void;
}

function loadClosed(): Set<string> {
  try { const v = JSON.parse(localStorage.getItem(CLOSED_KEY) ?? '[]'); return new Set(Array.isArray(v) ? v.filter((x) => typeof x === 'string') : []); }
  catch { return new Set(); }
}

function download(text: string, name: string) {
  const url = URL.createObjectURL(new Blob([text], { type: 'application/json' }));
  const a = document.createElement('a');
  a.href = url;
  a.download = name;
  a.click();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}

const kb = (n: number) => (n < 1024 ? `${n} B` : `${(n / 1024).toFixed(n < 10240 ? 1 : 0)} KB`);

/** The Library side tab: the IDE's own collection of programs, in folders, with search, selection, export/import. */
export function LibraryPanel({ core, lib, problem, change, onOpen, board, onUpload, onFromBoard }: Props) {
  const [query, setQuery] = useState('');
  const [selected, setSelected] = useState<Set<string>>(new Set());
  const [closed, setClosed] = useState<Set<string>>(loadClosed);
  const [msg, setMsg] = useState<{ text: string; bad: boolean } | null>(null);
  const [dialog, setDialog] = useState<null | { kind: 'import' } | { kind: 'move'; ids: string[] } | { kind: 'move-folder'; path: string }>(null);

  useEffect(() => { try { localStorage.setItem(CLOSED_KEY, JSON.stringify([...closed])); } catch { /* not remembered */ } }, [closed]);

  const tree = useMemo(() => buildTree(lib), [lib]);
  const hits = useMemo(() => search(lib, query), [lib, query]);
  const sel = lib.programs.filter((p) => selected.has(p.id));       // ids that still exist

  const run = (f: (l: Library) => Library, ok?: string) => {
    const err = change(f);
    setMsg(err ? { text: err, bad: true } : ok ? { text: ok, bad: false } : null);
    return !err;
  };

  const toggleOpen = (path: string) => setClosed((c) => { const n = new Set(c); if (n.has(path)) n.delete(path); else n.add(path); return n; });
  const toggleSel = (ids: string[], on: boolean) => setSelected((s) => { const n = new Set(s); for (const i of ids) { if (on) n.add(i); else n.delete(i); } return n; });

  const newProgram = (folder: string) => {
    const name = prompt(`Name of the new program${folder ? ` in ${folder}` : ''} (a-z 0-9 - _, starting with a letter, up to 15):`)?.trim();
    if (!name) return;
    let made: LocalProgram | undefined;
    if (run((l) => { const r = createProgram(l, folder, name, NEW_SOURCE(name)); made = r.program; return r.lib; }) && made) {
      if (folder) setClosed((c) => { const n = new Set(c); for (let f = folder; f; f = parentFolder(f)) n.delete(f); return n; });
      onOpen(made);
    }
  };
  const newFolder = (parent: string) => {
    const name = prompt(`Name of the new folder${parent ? ` in ${parent}` : ''} (a / makes folders inside folders):`)?.trim();
    if (name) run((l) => addFolder(l, joinFolder(parent, name)), `folder ${joinFolder(parent, normFolderSafe(name))} created`);
  };
  const renameFolderAsk = (path: string) => {
    const name = prompt(`New name for the folder ${path}:`, folderName(path))?.trim();
    if (name && name !== folderName(path)) run((l) => renameFolder(l, path, joinFolder(parentFolder(path), name)));
  };
  const deleteFolderAsk = (path: string) => {
    const s = folderStats(lib, path);
    const what = s.programs || s.folders ? ` and everything in it (${s.programs} program${s.programs === 1 ? '' : 's'}, ${s.folders} folder${s.folders === 1 ? '' : 's'})` : '';
    if (confirm(`Delete the folder ${path}${what}? This can't be undone (export first to keep a copy).`)) run((l) => deleteFolder(l, path), `deleted ${path}`);
  };
  const renameProgramAsk = (p: LocalProgram) => {
    const name = prompt(`New name for ${programPath(p)} (a-z 0-9 - _, starting with a letter, up to 15):`, p.name)?.trim();
    if (name && name !== p.name) run((l) => renameProgram(l, p.id, name));
  };
  const duplicate = (p: LocalProgram) => {
    let made: LocalProgram | undefined;
    if (run((l) => {
      const name = uniqueName(p.name, new Set(l.programs.filter((q) => q.folder === p.folder).map((q) => q.name)));
      const r = createProgram(l, p.folder, name, p.source); made = r.program; return r.lib;
    })) setMsg({ text: `copied to ${programPath(made!)}`, bad: false });
  };
  const deleteAsk = (ps: LocalProgram[]) => {
    if (!ps.length) return;
    const what = ps.length === 1 ? programPath(ps[0]) : `${ps.length} programs`;
    if (confirm(`Delete ${what} from the local library? This can't be undone (export first to keep a copy).`)) {
      if (run((l) => deletePrograms(l, ps.map((p) => p.id)), `deleted ${what}`)) toggleSel(ps.map((p) => p.id), false);
    }
  };
  const exportAll = () => {
    if (!lib.programs.length && !lib.folders.length) { setMsg({ text: 'the library is empty', bad: true }); return; }
    download(JSON.stringify(exportLibrary(lib, null), null, 2), exportFileName());
    setMsg({ text: `exported ${lib.programs.length} programs`, bad: false });
  };
  const exportSel = () => {
    download(JSON.stringify(exportLibrary(lib, sel.map((p) => p.id)), null, 2), exportFileName());
    setMsg({ text: `exported ${sel.length} program${sel.length === 1 ? '' : 's'}`, bad: false });
  };

  const subtreeIds = (n: FolderNode): string[] => lib.programs.filter((p) => inFolder(p.folder, n.path)).map((p) => p.id);

  const programRow = (p: LocalProgram, depth: number, extra?: React.ReactNode) => (
    <li key={p.id} className="lib-row" style={{ paddingLeft: depth * 14 }}>
      <input type="checkbox" checked={selected.has(p.id)} onChange={(e) => toggleSel([p.id], e.target.checked)} aria-label={`select ${programPath(p)}`} />
      <span className="lib-icon" aria-hidden>❖</span>
      <button className="link lib-name" onClick={() => onOpen(p)} title={`Open ${programPath(p)} (${p.source.length} characters)`}>
        {extra ? programPath(p) : p.name}
      </button>
      {extra}
      <span className="lib-actions">
        <button onClick={() => renameProgramAsk(p)} title="Rename">✎</button>
        <button onClick={() => setDialog({ kind: 'move', ids: [p.id] })} title="Move to another folder">⇢</button>
        <button onClick={() => duplicate(p)} title="Duplicate">⧉</button>
        <button onClick={() => deleteAsk([p])} title="Delete">✕</button>
      </span>
    </li>
  );

  const folderRows = (n: FolderNode, depth: number): React.ReactNode[] => {
    const rows: React.ReactNode[] = [];
    for (const f of n.folders) {
      const ids = subtreeIds(f), nsel = ids.filter((i) => selected.has(i)).length, open = !closed.has(f.path);
      rows.push(
        <li key={'f:' + f.path} className="lib-row lib-folder" style={{ paddingLeft: depth * 14 }}>
          <input type="checkbox" checked={ids.length > 0 && nsel === ids.length} disabled={!ids.length}
            ref={(el) => { if (el) el.indeterminate = nsel > 0 && nsel < ids.length; }}
            onChange={() => toggleSel(ids, nsel < ids.length)} aria-label={`select everything in ${f.path}`} />
          <button className="link lib-toggle" onClick={() => toggleOpen(f.path)} aria-expanded={open} title={f.path}>
            {open ? '▾' : '▸'} <span className="lib-icon" aria-hidden>📁</span> {f.name}
          </button>
          <span className="muted small">{ids.length || ''}</span>
          <span className="lib-actions">
            <button onClick={() => newProgram(f.path)} title="New program in this folder">+</button>
            <button onClick={() => newFolder(f.path)} title="New folder inside">+📁</button>
            <button onClick={() => renameFolderAsk(f.path)} title="Rename">✎</button>
            <button onClick={() => setDialog({ kind: 'move-folder', path: f.path })} title="Move into another folder">⇢</button>
            <button onClick={() => deleteFolderAsk(f.path)} title="Delete the folder and everything in it">✕</button>
          </span>
        </li>,
      );
      if (open) rows.push(...folderRows(f, depth + 1));
    }
    for (const p of n.programs) rows.push(programRow(p, depth));
    return rows;
  };

  return (
    <div className="library">
      <input className="search" type="search" placeholder="Search names, folders and code" value={query}
        onChange={(e) => setQuery(e.target.value)} aria-label="Search the local library" />
      <div className="row wrap">
        <button onClick={() => newProgram('')}>+ Program</button>
        <button onClick={() => newFolder('')}>+ Folder</button>
        <span className="spacer" />
        <button onClick={onFromBoard} disabled={!board}
          title={board ? `Save ${board.name}'s library and channel programs here` : 'Connect to a board first'}>From board…</button>
        <button onClick={() => setDialog({ kind: 'import' })} title="Import a library file (from this computer or a web address)">Import…</button>
        <button onClick={exportAll} title="Download the whole library as a JSON file">Export all</button>
      </div>
      {sel.length > 0 && (
        <div className="lib-selbar row wrap">
          <span>{sel.length} selected</span>
          <button onClick={() => onUpload(sel.map((p) => p.id))} disabled={!board}
            title={board ? `Store the selected programs in ${board.name}'s library` : 'Connect to a board first'}>Upload to {board?.name ?? 'board'}…</button>
          <button onClick={() => setDialog({ kind: 'move', ids: sel.map((p) => p.id) })}>Move to…</button>
          <button onClick={exportSel}>Export</button>
          <button onClick={() => deleteAsk(sel)}>Delete</button>
          <button className="link" onClick={() => setSelected(new Set())}>clear</button>
        </div>
      )}
      {problem && <div className="banner">{problem}</div>}
      {msg && <div className={msg.bad ? 'bad small' : 'muted small'}>{msg.text}</div>}
      {query.trim() ? (
        <ul className="lib-tree">
          {hits.length === 0 && <li className="muted">Nothing matches.</li>}
          {hits.map((h) => programRow(h.program, 0,
            <span className="muted small lib-hit">{h.in === 'code' ? <>line {h.lineNo}: <code>{h.line}</code></> : `(${h.in})`}</span>))}
        </ul>
      ) : (
        <ul className="lib-tree">
          {!lib.programs.length && !lib.folders.length && <li className="muted">Empty. Make a program with + Program, save an
            editor tab with “Save to local library”, or import a library file.</li>}
          {folderRows(tree, 0)}
        </ul>
      )}
      <div className="muted small">{lib.programs.length} program{lib.programs.length === 1 ? '' : 's'}, {lib.folders.length} folder{lib.folders.length === 1 ? '' : 's'} · {kb(librarySize(lib))} in this browser’s storage</div>

      {dialog?.kind === 'import' && <ImportDialog core={core} lib={lib} onClose={() => setDialog(null)}
        onImport={(text, target, mode) => {
          let summary = '';
          const err = change((l) => {
            const r = applyImport(l, planImport(l, parseLibraryFile(text), target), mode);
            summary = [r.added && `${r.added} added`, r.replaced && `${r.replaced} replaced`, r.renamed && `${r.renamed} kept as copies`,
              r.skipped && `${r.skipped} skipped`, r.unchanged && `${r.unchanged} already there`].filter(Boolean).join(', ') || 'nothing to import';
            return r.lib;
          });
          if (!err) { setMsg({ text: `imported: ${summary}`, bad: false }); setDialog(null); }
          return err;
        }} />}
      {dialog?.kind === 'move' && <FolderDialog title={`Move ${dialog.ids.length === 1 ? programPath(lib.programs.find((p) => p.id === dialog.ids[0]) ?? { folder: '', name: '?' }) : `${dialog.ids.length} programs`} to`}
        lib={lib} initial="" ok="Move" onClose={() => setDialog(null)}
        onOk={(f) => { if (run((l) => movePrograms(l, dialog.ids, f), `moved to ${f || 'the top level'}`)) setDialog(null); }} />}
      {dialog?.kind === 'move-folder' && <FolderDialog title={`Move the folder ${dialog.path} into`} lib={lib} initial={parentFolder(dialog.path)}
        exclude={dialog.path} ok="Move" onClose={() => setDialog(null)}
        onOk={(f) => { if (run((l) => renameFolder(l, dialog.path, joinFolder(f, folderName(dialog.path))))) setDialog(null); }} />}
    </div>
  );
}

function normFolderSafe(s: string) { try { return normFolder(s); } catch { return s; } }

/** A folder chooser: an existing folder (or the top level), or a new path typed in. */
export function FolderField({ lib, value, onChange, exclude }: { lib: Library; value: string; onChange(v: string): void; exclude?: string }) {
  const [typing, setTyping] = useState(false);
  const folders = lib.folders.filter((f) => !exclude || !inFolder(f, exclude));
  return (
    <span className="row">
      <select value={typing ? '\u0000' : folders.includes(value) || value === '' ? value : '\u0000'}
        onChange={(e) => { if (e.target.value === '\u0000') { setTyping(true); } else { setTyping(false); onChange(e.target.value); } }}>
        <option value="">(top level)</option>
        {folders.map((f) => <option key={f} value={f}>{'  '.repeat(f.split('/').length - 1)}{folderName(f)}</option>)}
        <option value={'\u0000'}>New folder…</option>
      </select>
      {(typing || (value !== '' && !folders.includes(value))) &&
        <input autoFocus value={value} onChange={(e) => onChange(e.target.value)} placeholder="e.g. clocks/wall" aria-label="new folder path" />}
    </span>
  );
}

function FolderDialog({ title, lib, initial, exclude, ok, onOk, onClose }:
  { title: string; lib: Library; initial: string; exclude?: string; ok: string; onOk(f: string): void; onClose(): void }) {
  const [f, setF] = useState(initial);
  return (
    <div className="modal-backdrop" onMouseDown={(e) => e.target === e.currentTarget && onClose()}>
      <div className="modal form" role="dialog" aria-label={title}>
        <div className="modal-head"><h2>{title}</h2><button onClick={onClose} aria-label="Close">✕</button></div>
        <label>Folder<FolderField lib={lib} value={f} onChange={setF} exclude={exclude} /></label>
        <div className="row"><span className="spacer" /><button onClick={onClose}>Cancel</button><button className="primary" onClick={() => onOk(f)}>{ok}</button></div>
      </div>
    </div>
  );
}

/** Save an editor tab's text into the local library under a name and folder (asks before replacing). */
export function SaveToLibraryDialog({ lib, initialName, source, onSave, onClose }:
  { lib: Library; initialName: string; source: string; onSave(folder: string, name: string): string | null; onClose(): void }) {
  const [name, setName] = useState(initialName);
  const [folder, setFolder] = useState('');
  const [err, setErr] = useState('');
  let existing: LocalProgram | undefined;
  try { existing = findProgram(lib, normFolder(folder), name.trim()); } catch { existing = undefined; }
  const save = () => {
    if (existing && existing.source !== source && !confirm(`${programPath(existing)} already exists. Replace its code?`)) return;
    const e = onSave(folder, name.trim());
    if (e) setErr(e); else onClose();
  };
  return (
    <div className="modal-backdrop" onMouseDown={(e) => e.target === e.currentTarget && onClose()}>
      <div className="modal form" role="dialog" aria-label="Save to local library">
        <div className="modal-head"><h2>Save to local library</h2><button onClick={onClose} aria-label="Close">✕</button></div>
        <label>Name (a-z 0-9 - _, starting with a letter, up to 15)
          <input autoFocus value={name} onChange={(e) => { setName(e.target.value); setErr(''); }} onKeyDown={(e) => e.key === 'Enter' && save()} />
        </label>
        <label>Folder<FolderField lib={lib} value={folder} onChange={(v) => { setFolder(v); setErr(''); }} /></label>
        {existing && <div className="muted small">{existing.source === source ? 'Already saved there with the same code.' : `Replaces the code of ${programPath(existing)}.`}</div>}
        {err && <div className="bad small">{err}</div>}
        <div className="row"><span className="spacer" /><button onClick={onClose}>Cancel</button><button className="primary" onClick={save}>{existing ? 'Replace' : 'Save'}</button></div>
      </div>
    </div>
  );
}

/** Import a library file: read it, show what it holds and what it would replace, pick the folder and the conflict mode. */
function ImportDialog({ core, lib, onImport, onClose }:
  { core: Core; lib: Library; onImport(text: string, target: string, mode: ImportMode): string | null; onClose(): void }) {
  const [text, setText] = useState<string | null>(null);
  const [fileName, setFileName] = useState('');
  const [err, setErr] = useState('');
  const [target, setTarget] = useState('');
  const [mode, setMode] = useState<ImportMode>('skip');
  const [url, setUrl] = useState('');
  const [fetching, setFetching] = useState(false);
  const fileRef = useRef<HTMLInputElement>(null);

  const parsed = useMemo(() => {
    if (text === null) return null;
    try { return { file: parseLibraryFile(text) }; } catch (e) { return { error: (e as Error).message }; }
  }, [text]);
  const plan = useMemo(() => {
    if (!parsed?.file) return null;
    try { return planImport(lib, parsed.file, target); } catch (e) { return { error: (e as Error).message }; }
  }, [parsed, lib, target]);
  const broken = useMemo(() => new Set(parsed?.file?.programs.filter((p) => core.check(p.source, 'render')).map((p) => programPath(p)) ?? []), [parsed, core]);

  const read = async (f: File | undefined) => {
    if (!f) return;
    setErr('');
    setFileName(f.name);
    try { setText(await f.text()); } catch (e) { setErr(`couldn't read ${f.name}: ${(e as Error).message}`); }
  };

  const fetchUrl = async () => {
    if (!url.trim()) return;
    setErr('');
    setFetching(true);
    try { const t = await fetchLibraryUrl(url); setFileName(url.trim()); setText(t); }
    catch (e) { setErr((e as Error).message); }
    finally { setFetching(false); }
  };

  const items = plan && 'items' in plan ? plan.items : [];
  const conflicts = items.filter((i) => i.existing && i.existing.source !== i.source).length;
  const same = items.filter((i) => i.existing && i.existing.source === i.source).length;

  return (
    <div className="modal-backdrop" onMouseDown={(e) => e.target === e.currentTarget && onClose()}>
      <div className="modal form lib-import" role="dialog" aria-label="Import a library file">
        <div className="modal-head"><h2>Import a library file</h2><button onClick={onClose} aria-label="Close">✕</button></div>
        <div className="row">
          <input ref={fileRef} type="file" accept=".json,application/json" hidden onChange={(e) => void read(e.target.files?.[0])} />
          <button onClick={() => fileRef.current?.click()}>Choose file…</button>
          <span className="muted small">{fileName || 'a .json file exported from a KTSN LED IDE'}</span>
        </div>
        <div className="row">
          <input type="url" className="grow" placeholder="or a web address: https://…/library.json" value={url} aria-label="library file address"
            onChange={(e) => setUrl(e.target.value)} onKeyDown={(e) => e.key === 'Enter' && void fetchUrl()} />
          <button onClick={() => void fetchUrl()} disabled={!url.trim() || fetching}>{fetching ? 'Fetching…' : 'Fetch'}</button>
        </div>
        {err && <div className="bad small">{err}</div>}
        {parsed?.error && <div className="bad small">{parsed.error}</div>}
        {parsed?.file && <>
          <div className="small">{parsed.file.programs.length} program{parsed.file.programs.length === 1 ? '' : 's'}
            {parsed.file.folders.length ? `, ${parsed.file.folders.length} folder${parsed.file.folders.length === 1 ? '' : 's'}` : ''}
            {broken.size ? <span className="bad"> · {broken.size} don’t compile here (imported anyway)</span> : null}</div>
          <label>Into folder<FolderField lib={lib} value={target} onChange={setTarget} /></label>
          {plan && 'error' in plan && <div className="bad small">{plan.error}</div>}
          <ul className="lib-import-list">
            {items.map((i) => {
              const p = programPath({ folder: i.folder, name: i.name });
              const rel = programPath({ folder: i.folder.slice(target ? normFolderSafe(target).length + 1 : 0), name: i.name });
              return <li key={p}>
                <code>{p}</code>{' '}
                <span className="muted small">{!i.existing ? 'new' : i.existing.source === i.source ? 'already there' : 'exists, different code'}</span>
                {broken.has(rel) && <span className="bad small"> ✖ doesn’t compile</span>}
              </li>;
            })}
          </ul>
          {conflicts > 0 && (
            <div className="radio small">
              <span>{conflicts} name{conflicts === 1 ? '' : 's'} already used:</span>
              <label><input type="radio" checked={mode === 'skip'} onChange={() => setMode('skip')} /> skip them</label>
              <label><input type="radio" checked={mode === 'overwrite'} onChange={() => setMode('overwrite')} /> replace</label>
              <label><input type="radio" checked={mode === 'keep'} onChange={() => setMode('keep')} /> keep both</label>
            </div>
          )}
          {same > 0 && <div className="muted small">{same} identical program{same === 1 ? ' is' : 's are'} already there and won’t change.</div>}
        </>}
        <div className="row"><span className="spacer" /><button onClick={onClose}>Cancel</button>
          <button className="primary" disabled={!parsed?.file || !plan || 'error' in plan} onClick={() => setErr(onImport(text!, target, mode) ?? '')}>Import</button></div>
      </div>
    </div>
  );
}
