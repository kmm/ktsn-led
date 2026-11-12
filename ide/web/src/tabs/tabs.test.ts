import { describe, expect, it } from 'vitest';
import { findTab, isUnsent, LIB_NAME, loadTabs, newTab, tabTitle, type EditorTab } from './tabs';

const store = (o: Record<string, unknown>) => ({ getItem: (k: string) => (k in o ? JSON.stringify(o[k]) : null) });

describe('editor tabs', () => {
  it('migrates the single editor and preview autoexec of before', () => {
    const s = loadTabs(store({ 'ktsn-led.ide.source': '(fill (buf O) #FF0000)', 'ktsn-led.ide.autoexec': '(set (buf G0) 0 #FFFFFF)',
      'ktsn-led.ide.prefs': { deviceId: 'dev1', channel: 1, sendOnSave: true } }), 'EXAMPLE');
    expect(s.tabs.map((t) => [t.kind, t.deviceId, t.ch, t.source])).toEqual([
      ['program', 'dev1', 1, '(fill (buf O) #FF0000)'], ['autoexec', 'dev1', 0, '(set (buf G0) 0 #FFFFFF)']]);
    expect(s.activeId).toBe(s.tabs[0].id);
  });
  it('starts with the example on a first run, and skips an empty old autoexec', () => {
    const s = loadTabs(store({ 'ktsn-led.ide.autoexec': '  ' }), 'EXAMPLE');
    expect(s.tabs).toHaveLength(1);
    expect(s.tabs[0]).toMatchObject({ kind: 'program', deviceId: null, source: 'EXAMPLE' });
  });
  it('restores saved tabs, and repairs a missing active tab', () => {
    const tabs: EditorTab[] = [{ id: 'a', kind: 'program', deviceId: null, ch: 0, source: 'x' }];
    expect(loadTabs(store({ 'ktsn-led.ide.tabs': { tabs, activeId: 'gone' } }), 'E').activeId).toBe('a');
    expect(loadTabs(store({ 'ktsn-led.ide.tabs': { tabs: [], activeId: 'a' } }), 'E').tabs[0].source).toBe('E');   // invalid: start over
  });
  it('names and finds tabs', () => {
    const t = newTab('program', []);
    expect(t.name).toBe('untitled 1');
    expect(newTab('autoexec', [newTab('autoexec', [])]).name).toBe('autoexec 2');
    const names = (id: string) => ({ d1: 'Kitchen' } as Record<string, string>)[id];
    expect(tabTitle({ ...t, deviceId: 'd1', ch: 2 }, names)).toBe('Kitchen · ch2');
    expect(tabTitle({ ...t, kind: 'autoexec', deviceId: 'd1' }, names)).toBe('Kitchen · autoexec');
    expect(tabTitle({ ...t, deviceId: 'zz' }, names)).toBe('unknown board · ch0');
    const list = [{ ...t, deviceId: 'd1', ch: 1 }, { ...t, id: 'q', kind: 'autoexec' as const, deviceId: 'd1' }];
    expect(findTab(list, 'program', 'd1', 1)).toBe(list[0]);
    expect(findTab(list, 'program', 'd1', 0)).toBeUndefined();
    expect(findTab(list, 'autoexec', 'd1')).toBe(list[1]);
    const libs = [{ ...t, id: 'l1', kind: 'library' as const, deviceId: 'd1', lib: 'sunset' }, { ...t, id: 'l2', kind: 'library' as const, deviceId: 'd1', lib: 'chase' }];
    expect(findTab(libs, 'library', 'd1', 0, 'chase')).toBe(libs[1]);
    expect(tabTitle(libs[0], names)).toBe('Kitchen · lib sunset');
    expect(LIB_NAME.test('sunset-2') && !LIB_NAME.test('Sunset') && !LIB_NAME.test('2fast') && !LIB_NAME.test('a'.repeat(16))).toBe(true);
  });
  it('marks board tabs whose text differs from the board', () => {
    const t: EditorTab = { id: 'a', kind: 'program', deviceId: 'd1', ch: 0, source: 'x' };
    expect(isUnsent(t)).toBe(true);                               // never sent
    expect(isUnsent({ ...t, boardSource: 'x' })).toBe(false);
    expect(isUnsent({ ...t, boardSource: 'y' })).toBe(true);
    expect(isUnsent({ ...t, deviceId: null })).toBe(false);       // preview-only tabs are never "unsent"
  });
  it('local library tabs: titled by their library path, the dot means unsaved, restored from storage', () => {
    const t: EditorTab = { id: 'l', kind: 'local', deviceId: null, ch: 0, source: 'a', boardSource: 'a', localId: 'p1', name: 'wall' };
    expect(tabTitle(t, () => undefined, (id) => (id === 'p1' ? 'clocks/wall' : undefined))).toBe('clocks/wall');
    expect(tabTitle(t, () => undefined)).toBe('wall');
    expect(isUnsent(t)).toBe(false);
    expect(isUnsent({ ...t, source: 'b' })).toBe(true);
    expect(loadTabs(store({ 'ktsn-led.ide.tabs': { tabs: [t], activeId: 'l' } }), 'EXAMPLE').tabs[0]).toEqual(t);
    expect(loadTabs(store({ 'ktsn-led.ide.tabs': { tabs: [{ ...t, localId: undefined }], activeId: 'l' } }), 'EXAMPLE').tabs[0].source).toBe('EXAMPLE');
  });
});
