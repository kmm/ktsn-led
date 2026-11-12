// The IDE's saved state moved from `ledctl.ide.*` to `ktsn-led.ide.*` with the project's rename (0.18.0). Imported
// first in main.tsx, so it runs before any module reads its key. Old keys are copied, not moved: an older IDE build
// on the same address still finds its state.

export const OLD_PREFIX = 'ledctl.ide.', NEW_PREFIX = 'ktsn-led.ide.';

/** Copy each old key to its new name, unless the new one already exists. Returns how many were copied. */
export function renameStorage(s: Storage): number {
  let n = 0;
  const old: string[] = [];
  for (let i = 0; i < s.length; i++) { const k = s.key(i); if (k?.startsWith(OLD_PREFIX)) old.push(k); }
  for (const k of old) {
    const to = NEW_PREFIX + k.slice(OLD_PREFIX.length), v = s.getItem(k);
    if (v !== null && s.getItem(to) === null) { s.setItem(to, v); n++; }
  }
  return n;
}

try { renameStorage(localStorage); } catch { /* storage blocked: nothing to carry over */ }
