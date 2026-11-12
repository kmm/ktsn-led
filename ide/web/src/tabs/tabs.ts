// Editor tabs: each has its own source and a target (preview only, a board channel, a board's autoexec, a program
// in a board's library, or a program in the IDE's local library).
// Pure functions + localStorage; the UI lives in App / TabBar.

export type TabKind = 'program' | 'autoexec' | 'library' | 'automation' | 'local';

export interface EditorTab {
  id: string;
  kind: TabKind;
  deviceId: string | null;        // null: preview only
  ch: number;                     // program tabs: channel on the device
  source: string;
  /** What the board had when this tab was opened from it or last sent; undefined if never. Drives the "unsent" dot.
   *  Local tabs: what the local library holds (the dot means unsaved). */
  boardSource?: string | null;
  name?: string;                  // preview-only tabs: "untitled 2"
  lib?: string;                   // library tabs: the program's name in the board's library
  localId?: string;               // local tabs: the program's id in the local library
}

export interface TabsState { tabs: EditorTab[]; activeId: string }

const KEY = 'ktsn-led.ide.tabs';
const OLD_SOURCE = 'ktsn-led.ide.source', OLD_AUTOEXEC = 'ktsn-led.ide.autoexec', OLD_PREFS = 'ktsn-led.ide.prefs';

export const newTabId = () => Math.random().toString(36).slice(2, 10);

export function newTab(kind: TabKind, existing: EditorTab[], init: Partial<EditorTab> = {}): EditorTab {
  const n = existing.filter((t) => t.deviceId === null && t.kind === kind).length + 1;
  return { id: newTabId(), kind, deviceId: null, ch: 0, source: '', name: kind === 'program' ? `untitled ${n}` : `${kind} ${n}`, ...init };
}

/** Tab label: the board target, the local library path, or the preview-only name. */
export function tabTitle(t: EditorTab, deviceName: (id: string) => string | undefined, localPath?: (id: string) => string | undefined): string {
  if (t.kind === 'local') return localPath?.(t.localId ?? '') ?? t.name ?? 'local program';
  if (t.kind === 'library') return `${t.deviceId === null ? '' : `${deviceName(t.deviceId) ?? 'unknown board'} · `}lib ${t.lib ?? '?'}`;
  if (t.deviceId === null) return t.name ?? (t.kind === 'program' ? 'untitled' : t.kind);
  const dev = deviceName(t.deviceId) ?? 'unknown board';
  return t.kind === 'program' ? `${dev} · ch${t.ch}` : `${dev} · ${t.kind}`;
}

/** Board-targeted and different from what the board was last known to have (local tabs: not saved to the library). */
export function isUnsent(t: EditorTab): boolean {
  if (t.kind === 'local') return (t.boardSource ?? null) !== t.source;
  return t.deviceId !== null && (t.boardSource ?? null) !== t.source;
}

/** The tab already open for this board target, if any (library tabs: by name). */
export function findTab(tabs: EditorTab[], kind: TabKind, deviceId: string, ch = 0, lib?: string): EditorTab | undefined {
  return tabs.find((t) => t.kind === kind && t.deviceId === deviceId &&
    (kind === 'autoexec' || kind === 'automation' || (kind === 'library' ? t.lib === lib : t.ch === ch)));
}

/** Library program names, as the board accepts them (progs.c lib_check_name). */
export const LIB_NAME = /^[a-z][a-z0-9_-]{0,14}$/;

function valid(s: unknown): s is TabsState {
  const v = s as TabsState;
  return !!v && Array.isArray(v.tabs) && v.tabs.length > 0 && typeof v.activeId === 'string' &&
    v.tabs.every((t) => typeof t?.id === 'string' && (t.kind === 'program' || t.kind === 'autoexec' || t.kind === 'automation' || (t.kind === 'library' && typeof t.lib === 'string') || (t.kind === 'local' && typeof t.localId === 'string')) && typeof t.source === 'string');
}

/** Saved tabs, or (first run after phase 5) tabs made from the single editor and preview autoexec of before. */
export function loadTabs(storage: Pick<Storage, 'getItem'>, example: string): TabsState {
  const read = (k: string) => { try { const v = storage.getItem(k); return v === null ? null : JSON.parse(v); } catch { return null; } };
  const saved = read(KEY);
  if (valid(saved)) {
    if (!saved.tabs.some((t) => t.id === saved.activeId)) saved.activeId = saved.tabs[0].id;
    return saved;
  }
  const prefs = (read(OLD_PREFS) ?? {}) as { deviceId?: string | null; channel?: number };
  const deviceId = typeof prefs.deviceId === 'string' ? prefs.deviceId : null;
  const prog: EditorTab = { id: newTabId(), kind: 'program', deviceId, ch: prefs.channel ?? 0,
    source: typeof read(OLD_SOURCE) === 'string' ? read(OLD_SOURCE) : example, name: 'untitled 1' };
  const tabs = [prog];
  const auto = read(OLD_AUTOEXEC);
  if (typeof auto === 'string' && auto.trim()) tabs.push({ id: newTabId(), kind: 'autoexec', deviceId, ch: 0, source: auto, name: 'autoexec 1' });
  return { tabs, activeId: prog.id };
}

export function saveTabs(storage: Pick<Storage, 'setItem'>, s: TabsState) {
  try { storage.setItem(KEY, JSON.stringify(s)); } catch { /* not remembered */ }
}
