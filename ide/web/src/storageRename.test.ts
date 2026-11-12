import { describe, expect, it } from 'vitest';
import { renameStorage } from './storageRename';

function mem(init: Record<string, string>): Storage {
  const m = new Map(Object.entries(init));
  return {
    get length() { return m.size; },
    key: (i) => [...m.keys()][i] ?? null,
    getItem: (k) => m.get(k) ?? null,
    setItem: (k, v) => { m.set(k, v); },
    removeItem: (k) => { m.delete(k); },
    clear: () => m.clear(),
  };
}

describe('storage rename', () => {
  it('copies old keys to the new names and keeps the old ones', () => {
    const s = mem({ 'ledctl.ide.tabs': 'T', 'ledctl.ide.palette.current': 'P', 'other': 'x' });
    expect(renameStorage(s)).toBe(2);
    expect(s.getItem('ktsn-led.ide.tabs')).toBe('T');
    expect(s.getItem('ktsn-led.ide.palette.current')).toBe('P');
    expect(s.getItem('ledctl.ide.tabs')).toBe('T');
    expect(s.getItem('ktsn-led.other')).toBeNull();
  });
  it('never overwrites state saved under the new name', () => {
    const s = mem({ 'ledctl.ide.tabs': 'old', 'ktsn-led.ide.tabs': 'new' });
    expect(renameStorage(s)).toBe(0);
    expect(s.getItem('ktsn-led.ide.tabs')).toBe('new');
  });
});
