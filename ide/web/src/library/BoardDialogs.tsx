import { useEffect, useMemo, useState } from 'react';
import type { Core, UseSources } from '../core/ktsnLed';
import { ApiError, type BoardChannel, type Transport } from '../devices/types';
import {
  boardEntries, boardFolder, type BoardEntry, entriesAsFile, planUpload, readBoardLibrary, runUpload, type UploadItem, type UploadMode,
  type UploadResult, uploadState, uploadSummary,
} from './board';
import { applyImport, type ImportMode, type Library, type LocalProgram, planImport } from './library';
import { FolderField } from './LibraryPanel';

const errText = (e: unknown) => (e instanceof Error ? e.message : String(e));
const UPLOAD_LABEL = { new: 'new', same: 'already there, same code', different: 'on the board, different code', exists: 'on the board' };

/** Upload a selection of local programs into the connected board's library (by name; folders don't go to the board). */
export function UploadDialog({ core, t, boardName, programs, onDone, onClose }: {
  core: Core; t: Transport; boardName: string; programs: LocalProgram[];
  /** After an upload: the results (App refreshes the board's library and its tabs). */
  onDone(results: UploadResult[], items: UploadItem[]): void;
  onClose(): void;
}) {
  const [board, setBoard] = useState<{ names: string[]; src: Map<string, string> } | null>(null);
  const [err, setErr] = useState('');
  const [off, setOff] = useState<Set<string>>(new Set());
  const [mode, setMode] = useState<UploadMode>('skip');
  const [busy, setBusy] = useState<number | null>(null);    // programs done so far while uploading
  const [results, setResults] = useState<UploadResult[] | null>(null);

  // The board's library with its code: to tell same from different, and for what the programs (use ...).
  useEffect(() => {
    let stop = false;
    (async () => {
      const names = (await t.libList()).programs.map((p) => p.name);
      const src = new Map<string, string>();
      for (const n of names) src.set(n, await t.libGet(n));
      if (!stop) setBoard({ names, src });
    })().catch((e) => { if (!stop) setErr(e instanceof ApiError && e.status === 404 ? 'this board\'s firmware has no program library (update it)' : errText(e)); });
    return () => { stop = true; };
  }, [t, programs]);

  const chosen = programs.filter((p) => !off.has(p.id));
  /** Compile as the board will: against its library, with the programs on their way there. */
  const checker = (progs: LocalProgram[]) => {
    const m: UseSources = new Map(board!.src);
    for (const p of progs) m.set(p.name, p.source);
    return (s: string) => core.checkUsing(s, m, 'render')?.text ?? null;
  };
  const plan = useMemo(() => {
    if (!board) return null;
    const p = planUpload(chosen, board.names, checker(chosen));
    for (const i of p.items) if (board.src.has(i.name)) i.boardSource = board.src.get(i.name);
    return p;
  }, [board, chosen.map((p) => p.id).join(), core]);  // eslint-disable-line react-hooks/exhaustive-deps
  const all = useMemo(() => (board ? planUpload(programs, board.names, checker(programs)).items : []), [board, programs, core]);  // eslint-disable-line react-hooks/exhaustive-deps
  const differ = plan?.items.filter((i) => uploadState(i) === 'different').length ?? 0;
  const sendable = plan ? plan.items.filter((i) => !i.broken).length : 0;

  const upload = async () => {
    if (!plan) return;
    setBusy(0);
    const rs = await runUpload(plan.items, mode, (name, src) => t.libPut(name, src), (_r, done) => setBusy(done));
    setBusy(null);
    setResults(rs);
    onDone(rs, plan.items);
  };

  const toggle = (id: string, on: boolean) => setOff((s) => { const n = new Set(s); if (on) n.delete(id); else n.add(id); return n; });

  return (
    <div className="modal-backdrop" onMouseDown={(e) => e.target === e.currentTarget && busy === null && onClose()}>
      <div className="modal form lib-import" role="dialog" aria-label={`Upload to ${boardName}`}>
        <div className="modal-head"><h2>Upload to {boardName}’s library</h2><button onClick={onClose} disabled={busy !== null} aria-label="Close">✕</button></div>
        {err ? <div className="bad small">{err}</div> : !board ? <div className="muted small">Reading {boardName}’s library…</div> : results ? <>
          <div className="small">{uploadSummary(results)}</div>
          <ul className="lib-import-list">
            {results.map((r) => <li key={r.path}><code>{r.path}</code>{r.path !== r.name && <span className="muted small"> → {r.name}</span>}{' '}
              <span className={r.outcome === 'failed' || r.outcome === 'not sent' ? 'bad small' : 'muted small'}>{r.outcome}{r.message ? `: ${r.message}` : ''}</span></li>)}
          </ul>
        </> : <>
          <div className="muted small">Programs are stored on the board under their names (folders stay here). The board keeps
            them over reboots; automation can <code>play</code> them.</div>
          <ul className="lib-import-list">
            {all.map((i) => {
              const on = !off.has(i.id), dup = on && plan!.duplicates.includes(i.name);
              return <li key={i.id}>
                <label className="check"><input type="checkbox" checked={on} disabled={busy !== null} onChange={(e) => toggle(i.id, e.target.checked)} />
                  <code>{i.path}</code></label>{' '}
                {i.path !== i.name && <span className="muted small">→ {i.name} </span>}
                <span className="muted small">{UPLOAD_LABEL[uploadState({ ...i, boardSource: board.src.get(i.name) })]}</span>
                {i.broken && <span className="bad small"> ✖ doesn’t compile (won’t be sent)</span>}
                {dup && <span className="bad small"> ✖ name chosen twice</span>}
              </li>;
            })}
          </ul>
          {plan!.duplicates.length > 0 && <div className="bad small">The board’s library has one program per name: choose only one
            of each {plan!.duplicates.join(', ')}.</div>}
          {differ > 0 && (
            <div className="radio small">
              <span>{differ} name{differ === 1 ? ' is' : 's are'} on the board with other code:</span>
              <label><input type="radio" checked={mode === 'skip'} onChange={() => setMode('skip')} /> skip them</label>
              <label><input type="radio" checked={mode === 'overwrite'} onChange={() => setMode('overwrite')} /> replace</label>
            </div>
          )}
          {busy !== null && <div className="muted small">Uploading… {busy} of {plan!.items.length}</div>}
        </>}
        <div className="row"><span className="spacer" />
          {results ? <button className="primary" onClick={onClose}>Close</button> : <>
            <button onClick={onClose} disabled={busy !== null}>Cancel</button>
            <button className="primary" disabled={!plan || busy !== null || plan.duplicates.length > 0 || !sendable} onClick={() => void upload()}>
              Upload {sendable || ''}</button>
          </>}
        </div>
      </div>
    </div>
  );
}

const IMPORT_LABEL = (e: { existing?: LocalProgram; source: string }) => (!e.existing ? 'new' : e.existing.source === e.source ? 'already there' : 'exists, different code');

/** Save the board's library programs and channel programs into the local library (default folder boards/<board>). */
export function FromBoardDialog({ t, boardName, channels, lib, preselect, change, onSaved, onClose }: {
  t: Transport; boardName: string; channels: BoardChannel[]; lib: Library;
  /** Only this entry ticked at first (e.g. "lib:wave"); all of them otherwise. */
  preselect?: string;
  change(f: (l: Library) => Library): string | null;
  onSaved(msg: string): void;
  onClose(): void;
}) {
  const [entries, setEntries] = useState<BoardEntry[] | null>(null);
  const [noLib, setNoLib] = useState(false);
  const [err, setErr] = useState('');
  const [on, setOn] = useState<Set<string> | null>(null);
  const [target, setTarget] = useState(() => { try { return boardFolder(boardName); } catch { return 'boards'; } });
  const [mode, setMode] = useState<ImportMode>('skip');

  useEffect(() => {
    let stop = false;
    readBoardLibrary(t).then((r) => {
      if (stop) return;
      const es = boardEntries(r.programs, channels);
      setNoLib(r.none);
      setEntries(es);
      setOn(new Set(preselect && es.some((e) => e.key === preselect) ? [preselect] : es.map((e) => e.key)));
    }, (e) => { if (!stop) setErr(errText(e)); });
    return () => { stop = true; };
  }, [t]);  // eslint-disable-line react-hooks/exhaustive-deps -- read once per dialog

  const chosen = entries?.filter((e) => on?.has(e.key)) ?? [];
  const plan = useMemo(() => {
    try { return planImport(lib, entriesAsFile(entries ?? []), target); } catch (e) { return { error: errText(e) }; }
  }, [lib, entries, target]);
  const items = 'items' in plan ? plan.items : [];
  const conflicts = items.filter((i, k) => on?.has(entries![k].key) && i.existing && i.existing.source !== i.source).length;

  const save = () => {
    let summary = '';
    const err = change((l) => {
      const r = applyImport(l, planImport(l, entriesAsFile(chosen), target), mode);
      summary = [r.added && `${r.added} added`, r.replaced && `${r.replaced} replaced`, r.renamed && `${r.renamed} kept as copies`,
        r.skipped && `${r.skipped} skipped`, r.unchanged && `${r.unchanged} already there`].filter(Boolean).join(', ') || 'nothing saved';
      return r.lib;
    });
    if (err) { setErr(err); return; }
    onSaved(`from ${boardName} into ${target.trim() || 'the top level'}: ${summary}`);
    onClose();
  };

  return (
    <div className="modal-backdrop" onMouseDown={(e) => e.target === e.currentTarget && onClose()}>
      <div className="modal form lib-import" role="dialog" aria-label={`Save from ${boardName}`}>
        <div className="modal-head"><h2>Save from {boardName} to the local library</h2><button onClick={onClose} aria-label="Close">✕</button></div>
        {err && <div className="bad small">{err}</div>}
        {!entries ? !err && <div className="muted small">Reading {boardName}’s programs…</div> : <>
          {noLib && <div className="muted small">This board’s firmware has no program library; only its channel programs are listed.</div>}
          {!entries.length ? <div className="muted small">{boardName} has no library programs and no channel programs.</div> : <>
            <div className="row small">
              <button className="link" onClick={() => setOn(new Set(entries.map((e) => e.key)))}>all</button>
              <button className="link" onClick={() => setOn(new Set())}>none</button>
            </div>
            <ul className="lib-import-list">
              {entries.map((e, k) => <li key={e.key}>
                <label className="check"><input type="checkbox" checked={!!on?.has(e.key)}
                  onChange={(ev) => setOn((s) => { const n = new Set(s); if (ev.target.checked) n.add(e.key); else n.delete(e.key); return n; })} />
                  {e.kind === 'library' ? <><span aria-hidden>☰</span> <code>{e.name}</code></> : <>{e.label} → <code>{e.name}</code></>}</label>{' '}
                {items[k] && <span className="muted small">{IMPORT_LABEL(items[k])}</span>}
              </li>)}
            </ul>
            <label>Into folder<FolderField lib={lib} value={target} onChange={setTarget} /></label>
            {'error' in plan && <div className="bad small">{plan.error}</div>}
            {conflicts > 0 && (
              <div className="radio small">
                <span>{conflicts} name{conflicts === 1 ? '' : 's'} already used there:</span>
                <label><input type="radio" checked={mode === 'skip'} onChange={() => setMode('skip')} /> skip them</label>
                <label><input type="radio" checked={mode === 'overwrite'} onChange={() => setMode('overwrite')} /> replace</label>
                <label><input type="radio" checked={mode === 'keep'} onChange={() => setMode('keep')} /> keep both</label>
              </div>
            )}
          </>}
        </>}
        <div className="row"><span className="spacer" /><button onClick={onClose}>Cancel</button>
          <button className="primary" disabled={!chosen.length || 'error' in plan} onClick={save}>Save {chosen.length || ''}</button></div>
      </div>
    </div>
  );
}

