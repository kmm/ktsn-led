import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { type CompileError, type Core, DEFAULT_WHITE, loadCore, type UseSources, type WhiteSettings } from './core/ktsnLed';
import type { BoardChannel } from './devices/types';
import { loadDevices, saveDevices, type DeviceSpec } from './devices/store';
import { chosenPorts, useConnection } from './devices/useConnection';
import { Editor } from './editor/Editor';
import { boardInputs } from './core/inputs';
import { Preview } from './preview/Preview';
import { findTab, isUnsent, LIB_NAME, loadTabs, newTab, saveTabs, tabTitle, type EditorTab, type TabKind, type TabsState } from './tabs/tabs';
import { useTheme } from './theme';
import { BoardPanel } from './ui/BoardPanel';
import { DeviceManager } from './ui/DeviceManager';
import { FlashDialog } from './ui/FlashDialog';
import { ReferencePanel } from './ui/ReferencePanel';
import { PalettePanel } from './palette/PalettePanel';
import { createProgram, findProgram, type Library, loadLibrary, type LocalProgram, normFolder, programById, programPath, saveLibrary, saveSource } from './library/library';
import { LibraryPanel, SaveToLibraryDialog } from './library/LibraryPanel';
import { FromBoardDialog, UploadDialog } from './library/BoardDialogs';
import { uploadSummary } from './library/board';
import { loadUses, localSources, usedNames } from './library/uses';
import type { PaletteTarget } from './palette/palette';
import { SideResizer, useSideWidth } from './ui/SideResizer';
import { TabBar } from './ui/TabBar';

const PREFS_KEY = 'ktsn-led.ide.prefs';
const PREVIEW_AUTOEXEC_KEY = 'ktsn-led.ide.previewAutoexec';
const OLD_AUTOEXEC_KEY = 'ktsn-led.ide.autoexec';
const EXAMPLE = `; Ctrl/Cmd+S runs the program in the preview (and sends it to the tab's board).
(def pal (set (slice (buf G0) 0 3) 0 #FF0000 #00FF00 #0000FF))
(fill-pat (buf O) pal (/ (env frame) 4) 3 t)
(scale (buf O) (+ 0.6 (* 0.4 (sin (* (env frame) 2)))))
`;
const MAX_CHANNELS = 8;

/** A board channel's wire format and white handling, for the preview (undefined on firmware without them). */
function whiteOf(c: BoardChannel | undefined): WhiteSettings | undefined {
  if (!c?.wire) return undefined;
  const mode = c.wmode === 'extract' || c.wmode === 'boost' ? c.wmode : 'direct';
  return { wire: c.wire, mode, point: c.wpoint ? parseInt(c.wpoint.slice(1), 16) : DEFAULT_WHITE.point, ratio: c.wratio ?? 1, max: c.wmax ?? 100 };
}

interface Prefs { deviceId: string | null; sendOnSave: boolean }

function load<T>(key: string, fallback: T): T {
  try { const v = localStorage.getItem(key); return v === null ? fallback : (JSON.parse(v) as T); } catch { return fallback; }
}
function store(key: string, v: unknown) {
  try { localStorage.setItem(key, JSON.stringify(v)); } catch { /* not remembered */ }
}

/** The preview's autoexec: the last saved autoexec tab (remembered), migrated from the old preview-only box. */
function loadPreviewAutoexec(): { src: string; from: string } | null {
  const v = load<{ src: string; from: string } | null>(PREVIEW_AUTOEXEC_KEY, null);
  if (v && typeof v.src === 'string') return v;
  const old = load<string>(OLD_AUTOEXEC_KEY, '');
  return typeof old === 'string' && old.trim() ? { src: old, from: 'autoexec 1' } : null;
}

export function App() {
  const [core, setCore] = useState<Core | null>(null);
  const [failed, setFailed] = useState('');
  const theme = useTheme();
  const [ts, setTs] = useState<TabsState>(() => loadTabs(localStorage, EXAMPLE));
  const [compileErr, setCompileErr] = useState<CompileError | null>(null);
  const [running, setRunning] = useState<{ src: string | null; version: number }>({ src: null, version: 0 });
  const [pAuto, setPAuto] = useState(() => { const a = loadPreviewAutoexec(); return a ? { ...a, version: 0 } : null; });
  const [previewMsg, setPreviewMsg] = useState('');
  const [boardMsg, setBoardMsg] = useState('');
  const [replace, setReplace] = useState<{ key: string; text: string; seq: number } | undefined>();
  const [devices, setDevices] = useState<DeviceSpec[]>(loadDevices);
  const [prefs, setPrefs] = useState<Prefs>(() => ({ deviceId: null, sendOnSave: true, ...load<Partial<Prefs>>(PREFS_KEY, {}) }));
  const [pendingConnect, setPendingConnect] = useState<string | null>(null);
  const [showDevices, setShowDevices] = useState(false);
  const [showFlash, setShowFlash] = useState(false);
  const [libVersion, setLibVersion] = useState(0);       // bumped when the IDE changed the board's library
  const [useVersion, setUseVersion] = useState(0);       // bumped when what (use ...) resolves to changed
  const [side, setSide] = useState<'preview' | 'board' | 'library' | 'reference' | 'palette'>('preview');
  const [libLoad] = useState(() => loadLibrary(localStorage));
  const [lib, setLib] = useState<Library>(libLoad.lib);
  const [libProblem, setLibProblem] = useState(libLoad.problem ?? '');
  const [localMsg, setLocalMsg] = useState('');
  const [saveDialog, setSaveDialog] = useState<{ tabId: string; name: string } | null>(null);
  // Local library <-> connected board: upload a selection, or save the board's programs here.
  const [boardDlg, setBoardDlg] = useState<null | { kind: 'upload'; programs: LocalProgram[] } | { kind: 'from'; preselect?: string }>(null);
  const [insert, setInsert] = useState<{ text: string; seq: number } | undefined>();
  // A palette opened from the code (▦): which tab and where, so the Palette tab can write it back.
  const [palLink, setPalLink] = useState<{ tabId: string; from: number; to: number; text: string; title: string } | null>(null);
  const [palRequest, setPalRequest] = useState<{ seq: number; stops: string[]; target: PaletteTarget } | undefined>();
  const [rangeEdit, setRangeEdit] = useState<{ key: string; from: number; to: number; expect: string; text: string; seq: number } | undefined>();
  const [palMsg, setPalMsg] = useState('');
  const [refSel, setRefSel] = useState<string | null>(null);
  const sideWidth = useSideWidth();
  const mainRef = useRef<HTMLElement>(null);

  const active = ts.tabs.find((t) => t.id === ts.activeId) ?? ts.tabs[0];
  const tsRef = useRef(ts);
  tsRef.current = ts;

  const device = useMemo(() => devices.find((d) => d.id === prefs.deviceId) ?? null, [devices, prefs.deviceId]);
  const conn = useConnection(device);
  const connected = conn.status === 'connected' && !!conn.transport;
  const connectedTo = (id: string | null) => connected && !!id && id === device?.id;
  const deviceName = useCallback((id: string) => devices.find((d) => d.id === id)?.name, [devices]);
  const localPath = useCallback((id: string) => { const p = programById(lib, id); return p && programPath(p); }, [lib]);
  const title = useCallback((t: EditorTab) => tabTitle(t, deviceName, localPath), [deviceName, localPath]);
  const libRef = useRef(lib);
  libRef.current = lib;

  /** Change the local library and store it. Returns the change's error or the storage problem, or null. */
  const changeLib = useCallback((f: (l: Library) => Library): string | null => {
    let next: Library;
    try { next = f(libRef.current); } catch (e) { return (e as Error).message; }
    const err = saveLibrary(localStorage, next);
    if (err) return err;
    libRef.current = next;
    setLib(next);
    setLibProblem('');
    return null;
  }, []);

  useEffect(() => { loadCore().then(setCore, (e) => setFailed(String(e))); }, []);
  useEffect(() => store(PREFS_KEY, prefs), [prefs]);
  useEffect(() => saveTabs(localStorage, ts), [ts]);
  useEffect(() => { if (pAuto) store(PREVIEW_AUTOEXEC_KEY, { src: pAuto.src, from: pAuto.from }); }, [pAuto]);

  // The preview runs the active program tab: on start and whenever another program tab becomes active.
  useEffect(() => {
    if (core && (active.kind === 'program' || active.kind === 'local')) setRunning((r) => ({ src: active.source, version: r.version + 1 }));
  }, [core, active.id]);  // eslint-disable-line react-hooks/exhaustive-deps

  // Connect once the header has switched to the board a tab's Connect button asked for.
  useEffect(() => {
    if (pendingConnect && device?.id === pendingConnect) { setPendingConnect(null); void conn.connect(); }
  }, [pendingConnect, device?.id, conn]);

  // (play ...) names in automation tabs are checked against the connected board's library (any name when offline).
  useEffect(() => {
    if (!core) return;
    const t = conn.transport;
    if (!t) { core.setLibrary(null); return; }
    let stop = false;
    t.libList().then((l) => { if (!stop) core.setLibrary(l.programs.map((p) => p.name)); }, () => { if (!stop) core.setLibrary(null); });
    return () => { stop = true; };
  }, [core, conn.transport, libVersion]);

  // (use ...): the connected board's library, or the local one when offline. Only what the open tabs reach is fetched.
  const useRoots = useMemo(() => [...new Set(ts.tabs.flatMap((t) => usedNames(t.source)))].sort().join('\n'), [ts.tabs]);
  const offlinePrograms = conn.transport ? null : lib.programs;
  useEffect(() => {
    if (!core) return;
    const apply = (m: UseSources) => {
      if (JSON.stringify([...m]) === JSON.stringify([...core.useSources()])) return;
      core.setUseSources(m);
      setUseVersion((v) => v + 1);
      setRunning((r) => (r.src !== null && usedNames(r.src).length ? { ...r, version: r.version + 1 } : r));
    };
    const t = conn.transport;
    if (!t) { apply(localSources(offlinePrograms ?? [])); return; }
    let stop = false;
    (async () => {
      const names = (await t.libList()).programs.map((p) => p.name);
      const m = await loadUses(useRoots ? useRoots.split('\n') : [], names, (n) => t.libGet(n));
      if (!stop) apply(m);
    })().catch(() => { if (!stop) apply(new Map()); });
    return () => { stop = true; };
  }, [core, conn.transport, libVersion, useRoots, offlinePrograms]);

  // The preview's input controls: the connected board's input slots (offline: the program's own names).
  const boardIns = useMemo(() => (conn.transport && conn.config?.io ? boardInputs(conn.config.io) : undefined), [conn.transport, conn.config]);
  // (io / in / analog ...) slots are checked against the connected board's (any slot and name when offline).
  useEffect(() => {
    if (!core) return;
    const io = conn.transport ? conn.config?.io : undefined;
    core.setOutputs(io ? io.map((o) => ({ n: o.n, name: o.name, mode: o.mode })) : null);
  }, [core, conn.transport, conn.config]);

  // Whenever the connected board's config is known, record what it runs, so tabs show whether they differ.
  useEffect(() => {
    const cfg = conn.config, id = device?.id;
    if (!cfg || !id) return;
    setTs((s) => {
      let changed = false;
      const tabs = s.tabs.map((t) => {
        if (t.deviceId !== id) return t;
        let b: string | null | undefined = t.boardSource;
        if (t.kind === 'program') { const c = cfg.channels[t.ch]; if (c?.running) b = c.prog ?? ''; }
        else if (t.kind === 'autoexec') b = cfg.autoexec ?? (t.source.trim() ? '' : t.source);   // no autoexec = an empty tab matches
        else if (t.kind === 'automation' && cfg.automation !== undefined) b = cfg.automation ?? (t.source.trim() ? '' : t.source);
        if (b === t.boardSource) return t;
        changed = true;
        return { ...t, boardSource: b };
      });
      return changed ? { ...s, tabs } : s;
    });
  }, [conn.config, device?.id]);

  // Local tabs follow the library: a deleted program leaves a preview-only tab; changed code (e.g. an import) is what
  // the tab is compared with.
  useEffect(() => {
    setTs((s) => {
      let changed = false;
      const tabs: EditorTab[] = [];
      for (const t of s.tabs) {
        if (t.kind !== 'local') { tabs.push(t); continue; }
        const p = programById(lib, t.localId ?? '');
        if (!p) { changed = true; tabs.push({ ...t, kind: 'program', localId: undefined, boardSource: undefined, name: `${t.name ?? 'program'} (deleted)` }); continue; }
        if (t.boardSource !== p.source || t.name !== p.name) { changed = true; tabs.push({ ...t, boardSource: p.source, name: p.name }); continue; }
        tabs.push(t);
      }
      return changed ? { ...s, tabs } : s;
    });
  }, [lib]);

  const updateTab = useCallback((id: string, patch: Partial<EditorTab>) =>
    setTs((s) => ({ ...s, tabs: s.tabs.map((t) => (t.id === id ? { ...t, ...patch } : t)) })), []);
  const activate = (id: string) => setTs((s) => ({ ...s, activeId: id }));
  const replaceText = (id: string, text: string) => setReplace((r) => ({ key: id, text, seq: (r?.seq ?? 0) + 1 }));

  const updateDevices = (list: DeviceSpec[]) => {
    setDevices(list);
    saveDevices(list);
    if (prefs.deviceId && !list.some((d) => d.id === prefs.deviceId)) setPrefs({ ...prefs, deviceId: null });
    // Tabs of a removed board become preview-only.
    setTs((s) => {
      const gone = (t: EditorTab) => t.deviceId !== null && !list.some((d) => d.id === t.deviceId);
      if (!s.tabs.some(gone)) return s;
      const tabs: EditorTab[] = [];
      for (const t of s.tabs) tabs.push(gone(t) ? { ...t, deviceId: null, boardSource: undefined, name: newTab(t.kind, tabs).name } : t);
      return { ...s, tabs };
    });
  };

  const onChange = useCallback((id: string, src: string) => updateTab(id, { source: src }), [updateTab]);
  const onLoad = useCallback((err: CompileError | null) => {
    setPreviewMsg(err ? `preview: not running (${err.text})` : `preview: running (${new Date().toLocaleTimeString()})`);
  }, []);

  /** Send a tab to its board (running program / autoexec only; not saved to flash). Checked locally first. */
  const send = async (t: EditorTab, src: string) => {
    if (!core || !conn.transport || !connectedTo(t.deviceId)) return;
    const where = title(t);
    const isAuto = t.kind === 'autoexec', isLib = t.kind === 'library', isAutomation = t.kind === 'automation';
    const err = (!isAuto && !isAutomation) || src.trim() ? core.check(src, isAutomation ? 'automation' : 'render') : null;
    if (err) { setBoardMsg(`not sent to ${where}: ${err.text}`); return; }
    if (t.kind === 'program' && !conn.config?.channels[t.ch]?.running) { setBoardMsg(`not sent: ${device!.name} has no running channel ${t.ch}`); return; }
    if (isLib && !src.trim()) { setBoardMsg(`not sent: an empty library program (delete it in the Board panel instead)`); return; }
    setBoardMsg(`sending to ${where}…`);
    try {
      if (isAuto) await conn.transport.patch({ autoexec: src.trim() ? src : null });
      else if (isAutomation) await conn.transport.patch({ automation: src.trim() ? src : null });
      else if (isLib) { await conn.transport.libPut(t.lib!, src); setLibVersion((v) => v + 1); }
      else await conn.transport.prog(t.ch, src);
      updateTab(t.id, { boardSource: src });
      if (isLib) { setBoardMsg(`stored in ${device!.name}'s library as ${t.lib} (${new Date().toLocaleTimeString()})`); return; }
      const what = isAuto ? (src.trim() ? 'autoexec ran' : 'autoexec cleared')
        : isAutomation ? (src.trim() ? 'automation runs now and every few seconds' : 'automation cleared') : 'running';
      setBoardMsg(`sent to ${where} (${new Date().toLocaleTimeString()}): ${what}, not saved to flash`);
      void conn.refreshConfig();
    } catch (e) {
      setBoardMsg(`${where}: ${(e as Error).message}`);
    }
  };

  /** Ctrl/Cmd+S and Run: the preview always; the tab's board too when connected to it and send-on-save is on. */
  const save = (src: string) => {
    const t = tsRef.current.tabs.find((x) => x.id === tsRef.current.activeId);
    if (!t) return;
    if (t.kind === 'autoexec') setPAuto((a) => ({ src, from: title(t), version: (a?.version ?? 0) + 1 }));
    else if (t.kind === 'automation') setPreviewMsg('automation: checked (it runs on the board, not in the preview)');
    else setRunning((r) => ({ src, version: r.version + 1 }));             // channel, board library and local programs
    if (t.kind === 'local') {
      const err = changeLib((l) => saveSource(l, t.localId!, src));
      if (!err) updateTab(t.id, { boardSource: src });
      setLocalMsg(err ? `not saved: ${err}` : `saved ${title(t)} to the local library (${new Date().toLocaleTimeString()})`);
      return;
    }
    if (prefs.sendOnSave) void send({ ...t, source: src }, src);
  };

  /** + menu: library programs need a board and a name. */
  const newTabOf = (kind: TabKind) => {
    if (kind !== 'library') { addTab(kind); return; }
    const dev = active.deviceId ?? prefs.deviceId;
    if (!dev) { alert('Pick a board first (Devices…): library programs are stored on a board.'); return; }
    const name = prompt('Name of the library program (a-z 0-9 - _, starting with a letter, up to 15):')?.trim();
    if (!name) return;
    if (!LIB_NAME.test(name)) { alert(`"${name}" isn't a valid name: a-z 0-9 - _, starting with a letter, up to 15 characters.`); return; }
    const t = findTab(ts.tabs, 'library', dev, 0, name);
    if (t) activate(t.id); else addTab('library', { deviceId: dev, lib: name, source: '; library program ' + name + '\n(fill (buf O) #000000)\n' });
  };

  const addTab = (kind: TabKind, init: Partial<EditorTab> = {}) => {
    const t = newTab(kind, ts.tabs, init);
    setTs((s) => ({ tabs: [...s.tabs, t], activeId: t.id }));
    return t;
  };

  const closeTab = (id: string) => {
    const t = ts.tabs.find((x) => x.id === id);
    if (!t) return;
    if (isUnsent(t) && !confirm(t.kind === 'local' ? `${title(t)} has changes not saved to the local library. Close it anyway?`
      : `${title(t)} differs from the board. Close it anyway?`)) return;
    setTs((s) => {
      const i = s.tabs.findIndex((x) => x.id === id);
      const rest = s.tabs.filter((x) => x.id !== id);
      if (!rest.length) { const n = newTab('program', []); return { tabs: [n], activeId: n.id }; }
      return { tabs: rest, activeId: s.activeId === id ? rest[Math.min(i, rest.length - 1)].id : s.activeId };
    });
  };

  /** Change the active tab's target. The board's copy is unknown until the next config read. */
  const retarget = (deviceId: string | null, ch: number) => {
    const others = ts.tabs.filter((x) => x.id !== active.id);
    updateTab(active.id, { deviceId, ch, boardSource: undefined,
      name: deviceId === null ? active.name ?? newTab(active.kind, others).name : active.name });
    if (deviceId && connectedTo(deviceId)) void conn.refreshConfig();
  };

  const connectTo = (id: string) => {
    if (device?.id === id) { void conn.connect(); return; }
    setPrefs({ ...prefs, deviceId: id });
    setPendingConnect(id);
  };

  /** Board panel Open: activate the tab for that target, or open a new one with the board's text. */
  const openFromBoard = (kind: TabKind, ch: number, src: string, lib?: string) => {
    if (!device) return;
    const t = findTab(ts.tabs, kind, device.id, ch, lib);
    if (!t) { addTab(kind, { deviceId: device.id, ch, source: src, boardSource: src, ...(lib ? { lib } : {}) }); setSide('preview'); return; }
    activate(t.id);
    if (t.source !== src && confirm(`${title(t)} differs from the board. Replace the tab's text with the board's?`)) {
      updateTab(t.id, { source: src, boardSource: src });
      replaceText(t.id, src);
      if (kind !== 'autoexec') setRunning((r) => ({ src, version: r.version + 1 }));
    }
    setSide('preview');
  };

  const openRef = useCallback((name: string) => { setRefSel(name || null); setSide('reference'); }, []);

  /** Local library: open a program in its tab (one tab per program). */
  const openLocal = (p: LocalProgram) => {
    const t = ts.tabs.find((x) => x.kind === 'local' && x.localId === p.id);
    if (t) activate(t.id);
    else addTab('local', { localId: p.id, source: p.source, boardSource: p.source, name: p.name });
    setSide('preview');
  };

  /** A name the library accepts, from a tab: its board library name, ch<N>, or the preview name made valid. */
  const suggestName = (t: EditorTab) => {
    if (t.kind === 'library' && t.lib) return t.lib;
    if (t.kind === 'program' && t.deviceId) return `ch${t.ch}`;
    const s = (t.name ?? 'program').toLowerCase().replace(/[^a-z0-9_-]+/g, '-').replace(/^[^a-z]+/, '').replace(/-+$/, '').slice(0, 15);
    return s || 'program';
  };

  /** Save to local library: a preview-only tab becomes the library program's tab; a board tab keeps its target. */
  const saveTabToLibrary = (tabId: string, folder: string, name: string): string | null => {
    const t = tsRef.current.tabs.find((x) => x.id === tabId);
    if (!t) return 'that tab was closed';
    let id = '';
    const err = changeLib((l) => {
      const f = normFolder(folder), existing = findProgram(l, f, name);
      if (existing) { id = existing.id; return saveSource(l, existing.id, t.source); }
      const r = createProgram(l, f, name, t.source);
      id = r.program.id;
      return r.lib;
    });
    if (err) return err;
    const path = programPath({ folder: normFolder(folder), name });
    if (t.kind === 'program' && t.deviceId === null) {
      const other = tsRef.current.tabs.find((x) => x.kind === 'local' && x.localId === id);
      if (other) setTs((s) => ({ tabs: s.tabs.filter((x) => x.id !== other.id), activeId: s.activeId === other.id ? t.id : s.activeId }));
      updateTab(t.id, { kind: 'local', localId: id, boardSource: t.source, name });
      setLocalMsg(`saved as ${path} in the local library; this tab now edits it`);
    } else setLocalMsg(`saved a copy as ${path} in the local library`);
    return null;
  };

  /** Reference examples: into one preview-only "example" tab (reused) and the preview; never sent to a board. */
  const tryExample = (src: string) => {
    const t = ts.tabs.find((x) => x.deviceId === null && x.kind === 'program' && x.name === 'example');
    if (t) {
      updateTab(t.id, { source: src });
      replaceText(t.id, src);
      if (t.id === active.id) setRunning((r) => ({ src, version: r.version + 1 }));
      else activate(t.id);
    } else addTab('program', { source: src, name: 'example' });
    setSide('preview');
  };

  const saveToFlash = async () => {
    if (!conn.transport) return;
    setBoardMsg('saving to flash…');
    try { await conn.transport.save(); setBoardMsg(`${device?.name}: saved to flash`); }
    catch (e) { setBoardMsg(`save failed: ${(e as Error).message}`); }
  };

  const openKeys = useMemo(() => ts.tabs.map((t) => t.id), [ts.tabs]);

  if (failed) return <div className="fatal">Could not load the DSL core: {failed}</div>;
  if (!core) return <div className="fatal muted">Loading…</div>;

  const dot = { idle: 'off', connecting: 'busy', connected: 'on', error: 'err' }[conn.status];
  const tabConnected = connectedTo(active.deviceId);
  const cfgChannels = tabConnected ? conn.config!.channels : null;
  const unsent = isUnsent(active);

  return (
    <div className="app">
      <header>
        <h1>KTSN LED</h1>
        <button className="primary" onClick={() => save(active.source)} title="Run in the preview and send to the tab's board (Ctrl/Cmd+S)">▶ Run</button>
        <span className="sep" />
        <select value={prefs.deviceId ?? ''} onChange={(e) => setPrefs({ ...prefs, deviceId: e.target.value || null })} title="Board to connect to">
          <option value="">No board</option>
          {devices.map((d) => <option key={d.id} value={d.id}>{d.name}</option>)}
        </select>
        {device && (connected
          ? <button onClick={conn.disconnect}><span className={`dot ${dot}`} /> Disconnect</button>
          : <button onClick={conn.connect} disabled={conn.status === 'connecting'}><span className={`dot ${dot}`} /> {conn.status === 'connecting' ? 'Connecting…' : 'Connect'}</button>)}
        {connected && (
          <>
            <label className="check" title="Ctrl/Cmd+S also sends the tab to its board (running program or autoexec; not saved to flash)">
              <input type="checkbox" checked={prefs.sendOnSave} onChange={(e) => setPrefs({ ...prefs, sendOnSave: e.target.checked })} /> send on save
            </label>
            <button onClick={saveToFlash} title="Store the board's programs and settings so they survive a reboot">Save to flash</button>
          </>
        )}
        <button onClick={() => setShowDevices(true)}>Devices…</button>
        <span className="spacer" />
        <button onClick={theme.cycle} title="Theme: system / light / dark">
          {theme.pref === 'system' ? '◐ System' : theme.pref === 'light' ? '☀ Light' : '☾ Dark'}
        </button>
      </header>
      <main ref={mainRef} style={{ '--side-w': `${sideWidth.width}px` } as React.CSSProperties}>
        <section className="pane editor-pane">
          <TabBar tabs={ts.tabs} activeId={active.id} title={title} onSelect={activate} onClose={closeTab} onNew={newTabOf} />
          <div className="target-bar">
            {active.kind === 'local' ? <>
              <span className="muted">Local library:</span> <code>{title(active)}</code>
              {unsent && <span className="muted" title="Ctrl/Cmd+S saves it">● not saved</span>}
              <span className="muted hint">Ctrl/Cmd+S saves it here and runs it in the preview. Rename or move it in the Library tab.</span>
            </> : <>
            <label>{active.kind === 'autoexec' ? 'Autoexec for' : active.kind === 'automation' ? 'Automation for' : active.kind === 'library' ? `Library program ${active.lib} on` : 'Target'}
              <select value={active.deviceId ?? ''} onChange={(e) => retarget(e.target.value || null, active.ch)}>
                {active.kind !== 'library' && <option value="">Preview only</option>}
                {devices.map((d) => <option key={d.id} value={d.id}>{d.name}</option>)}
              </select>
            </label>
            {active.kind === 'program' && active.deviceId && (
              <label>channel
                <select value={active.ch} onChange={(e) => retarget(active.deviceId, +e.target.value)}>
                  {Array.from({ length: Math.max(MAX_CHANNELS, active.ch + 1) }, (_, i) => {
                    const c = cfgChannels?.[i];
                    return <option key={i} value={i}>ch{i}{c ? (c.running ? ` (${c.npix} px)` : ' (not running)') : ''}</option>;
                  })}
                </select>
              </label>
            )}
            {active.deviceId && (tabConnected
              ? <button onClick={() => send(active, active.source)} title="Send this tab to the board now (not saved to flash)">Send</button>
              : <button onClick={() => connectTo(active.deviceId!)} disabled={conn.status === 'connecting'}>Connect</button>)}
            {unsent && <span className="muted" title="The board runs something else, or this tab was never sent">● differs from the board</span>}
            {active.kind === 'autoexec' && <span className="muted hint">Saving runs it in the preview too; empty clears the board's.</span>}
            {active.kind === 'automation' && <span className="muted hint">Runs on the board every few seconds (G buffers, env, play, stop, set-intensity); empty clears it. Not run in the preview.</span>}
            {active.kind === 'library' && <span className="muted hint">Sending stores it on the board (kept over reboots); the preview runs it.</span>}
            {(active.kind === 'program' || active.kind === 'library') &&
              <button onClick={() => setSaveDialog({ tabId: active.id, name: suggestName(active) })}
                title="Keep this program in the IDE's local library">Save to local library…</button>}
            </>}
          </div>
          <Editor core={core} dark={theme.dark} docKey={active.id} docText={active.source} openKeys={openKeys} recheck={useVersion}
            onChange={onChange} onCompile={setCompileErr} onSave={save} replace={replace} insert={insert} onOpenRef={openRef}
            onPalette={(p) => {
              setPalLink({ tabId: active.id, from: p.from, to: p.to, text: p.text, title: title(active) });
              setPalRequest((r) => ({ seq: (r?.seq ?? 0) + 1, stops: p.stops, target: p.target }));
              setPalMsg('');
              setSide('palette');
            }}
            rangeEdit={rangeEdit}
            onRangeEdited={(r) => {
              if (r) { setPalLink((l) => (l ? { ...l, ...r } : l)); setPalMsg(`applied (${new Date().toLocaleTimeString()})`); }
              else { setPalLink(null); setPalMsg(''); setBoardMsg('palette not applied: its code changed in the editor (open it again with ▦)'); }
            }}
            context={active.kind === 'automation' ? 'automation' : 'render'} />
        </section>
        <SideResizer state={sideWidth} container={mainRef} />
        <aside className="pane side-pane">
          <div className="tabs">
            <button className={side === 'preview' ? 'active' : ''} onClick={() => setSide('preview')}>Preview</button>
            <button className={side === 'board' ? 'active' : ''} onClick={() => setSide('board')} disabled={!device}>
              Board{device ? `: ${device.name}` : ''}
            </button>
            <button className={side === 'library' ? 'active' : ''} onClick={() => setSide('library')} title="Programs kept in this IDE (local library)">Library</button>
            <button className={side === 'palette' ? 'active' : ''} onClick={() => setSide('palette')} title="Build a palette and get its code">Palette</button>
            <button className={side === 'reference' ? 'active' : ''} onClick={() => setSide('reference')} title="Language reference (F1 on a function)">Reference</button>
          </div>
          <div className="tab-body" hidden={side !== 'preview'}>
            <Preview core={core} program={running.src} programVersion={running.version} onLoad={onLoad}
              npixHint={tabConnected && active.kind === 'program' ? conn.config!.channels[active.ch]?.npix : undefined}
              gammaHint={tabConnected && active.kind === 'program' ? conn.config!.channels[active.ch]?.gamma : undefined}
              ditherHint={tabConnected && active.kind === 'program' ? conn.config!.channels[active.ch]?.dither : undefined}
              refreshHint={tabConnected ? conn.config!.output?.refresh : undefined}
              chanIntensityHint={tabConnected && active.kind === 'program' ? conn.config!.channels[active.ch]?.intensity : undefined}
              locHint={tabConnected ? conn.config!.loc : undefined}
              whiteHint={whiteOf(tabConnected && active.kind === 'program' ? conn.config!.channels[active.ch] : undefined)}
              autoexec={pAuto} inputsHint={boardIns} />
          </div>
          <div className="tab-body" hidden={side !== 'board'}>
            <BoardPanel conn={conn} deviceName={device?.name ?? ''} visible={side === 'board'}
              onOpenProgram={(ch, src) => openFromBoard('program', ch, src)}
              onOpenAutoexec={(src) => openFromBoard('autoexec', 0, src)} onFlash={() => setShowFlash(true)}
              onOpenLibrary={(name, src) => openFromBoard('library', 0, src, name)} libVersion={libVersion}
              onOpenAutomation={(src) => openFromBoard('automation', 0, src)}
              onSaveToLocal={(preselect) => setBoardDlg({ kind: 'from', preselect })} />
          </div>
          <div className="tab-body" hidden={side !== 'library'}>
            <LibraryPanel core={core} lib={lib} problem={libProblem} change={changeLib} onOpen={openLocal}
              board={connected && device ? { name: device.name } : null}
              onUpload={(ids) => setBoardDlg({ kind: 'upload', programs: lib.programs.filter((p) => ids.includes(p.id)) })}
              onFromBoard={() => setBoardDlg({ kind: 'from' })} />
          </div>
          <div className="tab-body" hidden={side !== 'reference'}>
            <ReferencePanel selected={refSel} onSelect={setRefSel} onTry={tryExample} />
          </div>
          <div className="tab-body" hidden={side !== 'palette'}>
            <PalettePanel onInsert={(text) => setInsert((x) => ({ text, seq: (x?.seq ?? 0) + 1 }))}
              request={palRequest} linked={palLink && ts.tabs.some((t) => t.id === palLink.tabId) ? palLink.title : null}
              applyMsg={palMsg} onUnlink={() => { setPalLink(null); setPalMsg(''); }}
              onApply={(code) => {
                if (!palLink) return;
                if (active.id !== palLink.tabId) activate(palLink.tabId);
                setRangeEdit((x) => ({ key: palLink.tabId, from: palLink.from, to: palLink.to, expect: palLink.text, text: code, seq: (x?.seq ?? 0) + 1 }));
              }} />
          </div>
        </aside>
      </main>
      <footer>
        <span className={compileErr ? 'bad' : 'good'}>{compileErr ? `✖ ${compileErr.text}` : '✔ compiles'}</span>
        <span className="muted">{previewMsg}</span>
        {localMsg && <span className={/^not saved/.test(localMsg) ? 'bad' : 'muted'}>{localMsg}</span>}
        {device && <span className={conn.status === 'error' || /fail|not sent|line \d/.test(boardMsg) ? 'bad' : 'muted'}>
          {boardMsg || conn.message}</span>}
      </footer>
      {saveDialog && <SaveToLibraryDialog lib={lib} initialName={saveDialog.name}
        source={ts.tabs.find((t) => t.id === saveDialog.tabId)?.source ?? ''}
        onSave={(folder, name) => saveTabToLibrary(saveDialog.tabId, folder, name)} onClose={() => setSaveDialog(null)} />}
      {boardDlg?.kind === 'upload' && connected && device && <UploadDialog core={core} t={conn.transport!} boardName={device.name}
        programs={boardDlg.programs} onClose={() => setBoardDlg(null)}
        onDone={(rs, items) => {
          const sent = new Map(items.map((i) => [i.name, i.source]));
          const stored = new Set(rs.filter((r) => r.outcome === 'stored' || r.outcome === 'replaced').map((r) => r.name));
          if (stored.size) {
            setLibVersion((v) => v + 1);
            // Open board library tabs of those names now match what the board holds.
            setTs((s) => ({ ...s, tabs: s.tabs.map((t) => (t.kind === 'library' && t.deviceId === device.id && stored.has(t.lib ?? '')
              ? { ...t, boardSource: sent.get(t.lib!) } : t)) }));
          }
          setLocalMsg(`uploaded to ${device.name}: ${uploadSummary(rs)}`);
        }} />}
      {boardDlg?.kind === 'from' && connected && device && conn.config && <FromBoardDialog t={conn.transport!} boardName={device.name}
        channels={conn.config.channels} lib={lib} preselect={boardDlg.preselect} change={changeLib}
        onSaved={(m) => { setLocalMsg(`saved ${m}`); setSide('library'); }} onClose={() => setBoardDlg(null)} />}
      {showDevices && <DeviceManager devices={devices} onChange={updateDevices} onClose={() => setShowDevices(false)}
        onFlash={() => { setShowDevices(false); setShowFlash(true); }} />}
      {showFlash && (
        // A serial board in use is flashed through its own port (closed first: a port opens only once), then reconnected.
        <FlashDialog onClose={() => setShowFlash(false)}
          knownPort={device?.kind === 'serial' ? chosenPorts.get(device.id) ?? null : null}
          beforeFlash={async () => { if (device?.kind === 'serial' && conn.status !== 'idle') await conn.disconnect(); }}
          afterFlash={device && conn.status !== 'idle'
            ? () => void conn.reconnectAfterRestart('flashed, restarting…', device.kind === 'serial' ? 3000 : 10000) : undefined} />
      )}
    </div>
  );
}
