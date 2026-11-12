// What (use name) sees in the IDE: the connected board's library, or the local library when offline. Pure functions
// plus one async helper that takes the transport call it needs.

import type { UseSources } from '../core/ktsnLed';
import type { LocalProgram } from './library';

/** The names a program uses, without those in comments and strings. */
export function usedNames(src: string): string[] {
  const code = src.replace(/"(?:[^"\\\n]|\\.)*"|;.*$/gm, ' ');
  return [...new Set([...code.matchAll(/\(\s*use\s+([^\s();]+)/g)].map((m) => m[1]))];
}

/** Local programs by name. Folders don't exist for `use`, so a name in two folders can't be used. */
export function localSources(programs: LocalProgram[]): UseSources {
  const out: UseSources = new Map();
  for (const p of programs)
    out.set(p.name, out.has(p.name) ? { why: `'${p.name}' is in more than one folder of the local library` } : p.source);
  return out;
}

/** The board library programs reachable from `roots` through their own (use ...). Names the board doesn't have, or that
 *  can't be read, are left out (the compiler then says there is no such program). */
export async function loadUses(roots: string[], boardNames: string[], get: (name: string) => Promise<string>): Promise<UseSources> {
  const have = new Set(boardNames), out: UseSources = new Map(), todo = [...roots];
  for (let name = todo.pop(); name !== undefined; name = todo.pop()) {
    if (out.has(name) || !have.has(name)) continue;
    let src: string;
    try { src = await get(name); } catch { continue; }
    out.set(name, src);
    todo.push(...usedNames(src));
  }
  return out;
}

/** Items with the ones others use first (the board checks each program against what it already has). Otherwise the
 *  order stays; a circle is left for the compiler to report. */
export function usesFirst<T extends { name: string; source: string }>(items: T[]): T[] {
  const byName = new Map(items.map((i) => [i.name, i])), seen = new Set<T>(), out: T[] = [];
  const visit = (i: T) => {
    if (seen.has(i)) return;
    seen.add(i);
    for (const n of usedNames(i.source)) { const d = byName.get(n); if (d) visit(d); }
    out.push(i);
  };
  items.forEach(visit);
  return out;
}
