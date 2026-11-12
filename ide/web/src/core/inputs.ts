// Inputs the preview offers controls for: the connected board's input slots, or (offline) the names a program reads
// with (in x) / (analog x [samples]). Values go to the core with setInput (by slot or name).

export interface PreviewInput {
  key: string;                    // what setInput gets: the slot number as text (board) or the name (offline)
  label: string;
  kind: 'digital' | 'analog';
}

const USE = /\(\s*(in|analog)\s+([a-z][a-z0-9_-]{0,14}|\d+)(?=[\s)])/g;

/** Inputs a program reads by name or slot number, in order of first use (comments and strings are skipped). */
export function inputsInSource(src: string): PreviewInput[] {
  const code = src.replace(/"(?:[^"\\\n]|\\.)*"/g, '""').replace(/;[^\n]*/g, '');
  const seen = new Map<string, PreviewInput>();
  for (const m of code.matchAll(USE)) {
    const key = m[2];
    if (!seen.has(key)) seen.set(key, { key, label: /^\d+$/.test(key) ? `slot ${key}` : key, kind: m[1] === 'in' ? 'digital' : 'analog' });
  }
  return [...seen.values()];
}

/** The connected board's input slots. */
export function boardInputs(io: { n: number; name: string; mode: string }[]): PreviewInput[] {
  return io.filter((o) => o.mode === 'input' || o.mode === 'analog')
    .map((o) => ({ key: String(o.n), label: o.name || `slot ${o.n}`, kind: o.mode === 'input' ? 'digital' : 'analog' }));
}
